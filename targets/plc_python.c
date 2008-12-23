/*
 * Python Asynchronous execution code
 * 
 * PLC put python commands in a fifo, respecting execution order
 * with the help of C pragmas inserted in python_eval FB code 
 * 
 * Buffer content is read asynchronously, (from non real time part), 
 * commands are executed and result stored for later use by PLC.
 * 
 * In this implementation, fifo is a list of pointer to python_eval
 * function blocks structures. Some local variables have been added in
 * python_eval interface. We use those local variables as buffer and state
 * flags.
 * 
 * */

#include "iec_types_all.h"
#include "POUS.h"
#include <string.h>

/* The fifo (fixed size, as number of FB is fixed) */
static PYTHON_EVAL* EvalFBs[%(python_eval_fb_count)d];
/* Producer and consumer cursors */
static long Current_PLC_EvalFB;
static long Current_Python_EvalFB;

/* A global IEC-Python gateway state, for use inside python_eval FBs*/
static int PythonState;
#define PYTHON_LOCKED_BY_PYTHON 0
#define PYTHON_LOCKED_BY_PLC 1
#define PYTHON_MUSTWAKEUP 2
 
/* Each python_eval FunctionBlock have it own state */
#define PYTHON_FB_FREE 0
#define PYTHON_FB_REQUESTED 1
#define PYTHON_FB_PROCESSING 2
#define PYTHON_FB_ANSWERED 3

int WaitPythonCommands(void);
void UnBlockPythonCommands(void);
int TryLockPython(void);
void UnLockPython(void);
void LockPython(void);

void __init_python()
{
	int i;
	/* Initialize cursors */
	Current_Python_EvalFB = 0;
	Current_PLC_EvalFB = 0;
	PythonState = PYTHON_LOCKED_BY_PYTHON;
	for(i = 0; i < %(python_eval_fb_count)d; i++)
		EvalFBs[i] = NULL;
}

void __cleanup_python()
{
}

void __retrieve_python()
{
	/* Check Python thread is not being 
	 * modifying internal python_eval data */
	PythonState = TryLockPython() ? 
	                PYTHON_LOCKED_BY_PLC : 
	                PYTHON_LOCKED_BY_PYTHON;
	/* If python thread _is_ in, then PythonState remains PYTHON_LOCKED_BY_PYTHON 
	 * and python_eval will no do anything */
}

void __publish_python()
{
	if(PythonState & PYTHON_LOCKED_BY_PLC){
		/* If runnig PLC did push something in the fifo*/
		if(PythonState & PYTHON_MUSTWAKEUP){
			/* WakeUp python thread */
			UnBlockPythonCommands();
		}
		UnLockPython();
	}
}
/**
 * Called by the PLC, each time a python_eval 
 * FB instance is executed
 */
void __PythonEvalFB(PYTHON_EVAL* data__)
{
	/* python thread is not in ? */
	if( PythonState & PYTHON_LOCKED_BY_PLC){
		/* Rising edge on TRIG */
		if(data__->TRIG && !data__->TRIGM1 &&
		   /* and not already being processed */ 
		   data__->STATE == PYTHON_FB_FREE) 
		{
			/* Get a new line */
			Current_PLC_EvalFB = (Current_PLC_EvalFB + 1) %% %(python_eval_fb_count)d;
			/* Enter the block in the fifo
			/* Don't have to check if fifo cell is free
			 * as fifo size == FB count, and a FB cannot 
			 * be requested twice */
			EvalFBs[Current_PLC_EvalFB] = data__;
			/* copy CODE in variable into BUFFER local*/
			data__->BUFFER = data__->CODE;
			/* Set ACK pin to low so that we can set a rising edge on result */
			data__->ACK = 0;
			/* Mark FB busy */
			data__->STATE = PYTHON_FB_REQUESTED;
			/* Have to wakeup python thread in case he was asleep */
			PythonState |= PYTHON_MUSTWAKEUP;
			//printf("__PythonEvalFB push %%*s\n",data__->BUFFER.len, data__->BUFFER.body);
		}else if(data__->STATE == PYTHON_FB_ANSWERED){
			data__->RESULT = data__->BUFFER;
			data__->ACK = 1;
			data__->STATE = PYTHON_FB_FREE;
			//printf("__PythonEvalFB pop %%*s\n",data__->BUFFER.len, data__->BUFFER.body);
		}
		/* retain value for trig
		 * do this only when PYTHON_LOCKED_BY_PLC
		 * to avoid missed rising edge due to asynchronism */
		data__->TRIGM1 = data__->TRIG;
	}
}

char* PythonIterator(char* result)
{
	char* next_command;
	PYTHON_EVAL* data__;
	//printf("PythonIterator result %%s\n", result);
	/* take python mutex to prevent changing PLC data while PLC running */
	LockPython();
	/* Get current FB */
	data__ = EvalFBs[Current_Python_EvalFB];
	if(data__ && /* may be null at first run */
	   data__->STATE == PYTHON_FB_PROCESSING){ /* some answer awaited*/
	   	/* If result not None */
	   	if(result){
			/* Get results len */
			data__->BUFFER.len = strlen(result);
			/* prevent results overrun */
			if(data__->BUFFER.len > STR_MAX_LEN)
			{
				data__->BUFFER.len = STR_MAX_LEN;
				/* TODO : signal error */
			}
			/* Copy results to buffer */
			strncpy(data__->BUFFER.body, result, data__->BUFFER.len);
	   	}else{
	   		data__->BUFFER.len = 0;
	   	}
		/* remove block from fifo*/
		EvalFBs[Current_Python_EvalFB] = NULL;
		/* Mark block as answered */
		data__->STATE = PYTHON_FB_ANSWERED;
		/* Get a new line */
		Current_Python_EvalFB = (Current_Python_EvalFB + 1) %% %(python_eval_fb_count)d;
	}
	/* while next slot is empty */
	while(((data__ = EvalFBs[Current_Python_EvalFB]) == NULL) || 
	 	  /* or doesn't contain command */ 
	      data__->STATE != PYTHON_FB_REQUESTED)
	{
		UnLockPython();
		/* wait next FB to eval */
		WaitPythonCommands();
		LockPython();
	}
	/* Mark block as processing */
	data__->STATE = PYTHON_FB_PROCESSING;
	//printf("PythonIterator\n");
	/* make BUFFER a null terminated string */
	data__->BUFFER.body[data__->BUFFER.len] = 0;
	/* next command is BUFFER */
	next_command = data__->BUFFER.body;
	/* free python mutex */
	UnLockPython();
	/* return the next command to eval */
	return next_command;
}

