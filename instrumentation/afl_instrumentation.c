#include <sys/shm.h> // for shm functions
#include <stddef.h>  // for NULL
#include <unistd.h>  // for lseek, write, ftruncate

#include <utils.h>   // for FUZZ_* return values

#include <jansson_helper.h>  // for PARSE_OPTION_*

#include "afl_instrumentation.h"

// module level variables needed for callbacks
int shm_id;

/**
 * This function allocates and initializes a new instrumentation specific state
 * object based on the given options.
 * @param options - a JSON string that contains the instrumentation specific
 *                  string of options
 * @param state - an instrumentation specific JSON string previously returned
 *                from afl_get_state that should be loaded
 * @return - An instrumentation specific state object on success or NULL on failure
 */
void * afl_create(char *options, char *state) {
	afl_state_t *afl_state = setup_options(options);
	if(!afl_state)
		return NULL;

	if(state && afl_set_state(afl_state, state)) {
		DEBUG_MSG("Unable to set state for afl instrumentation");
		return NULL;
	}

	return afl_state;
}

/**
 * This function cleans up all resources with the passed in instrumentation state.
 * @param instrumentation_state - an instrumentation specific state object
 *                                previously created by the afl_create function
 *                                This state object should not be referenced after
 *                                this function returns.
 */
void afl_cleanup(void *instrumentation_state) {
	afl_state_t * state = (afl_state_t *)instrumentation_state;

	free(state->target_path);

	// Cleanup the fork server if necessary
	if(state->fork_server_setup) {
		fork_server_exit(&state->fs);
		state->fork_server_setup = 0;
	}
}

char * afl_get_state(void *instrumentation_state) {
	return NULL;
}

void afl_free_state(char *state) {
}

int afl_set_state(void *instrumentation_state, char *state) {
	return -1;
}

void * afl_merge(void *instrumentation_state, void *other_instrumentation_state) {
	return NULL;
}

/**
 * This function enables the instrumentation and runs the fuzzed process.
 * @param instrumentation_state - an instrumentation specific state object
 *                                previously created by the afl_create function
 * @process - a pointer to return a pid_t to the process that the
 *            instrumentation was enabled on
 * @cmd_line - the command line of the fuzzed process to enable instrumentation on
 * @input - a buffer to the input that should be sent to the fuzzed process
 * @input_length - the length of the input parameter
 * returns 0 on success, -1 on failure
 */
int afl_enable(void *instrumentation_state, pid_t *process, char *cmd_line,
		char *input, size_t input_length) {
	afl_state_t * state = (afl_state_t *)instrumentation_state;
	char ** argv;

	// If there's already a child process, get rid of it
	if(state->child_pid) {
		destroy_target_process(state);
	}

	if(create_target_process(state, cmd_line, input, input_length))
		return -1;
	state->process_finished = 0;
	state->fuzz_results_set = 0;

	*process = state->child_pid;
	return 0;
}

/**
 * This function determines if a new path was covered
 * @param instrumentation_state - an instrumentation specific state object
 *                                previously created by the afl_create function
 * @return - 1 if the previously setup process (via the enable function) took a
 *           new path, 0 if it did not, or -1 on failure.
 */
int afl_is_new_path(void *instrumentation_state) {
	afl_state_t * state = (afl_state_t *)instrumentation_state;

	// If we haven't set the fuzz results, do that and return the result
	if(!state->fuzz_results_set)
		finish_fuzz_round(state);
	if(state->last_is_new_path)
		return 1;
	return 0;
}

/**
 * This function will return the result of the fuzz job. It should be called
 * after the process has finished processing the tested input.  The target
 * process will also be cleaned up in the process.
 * @param instrumentation_state - an instrumentation specific structure
 *                                previously created by the afl_create function
 * @return - either FUZZ_NONE, FUZZ_HANG, FUZZ_CRASH, or -1 on error.
 */
int afl_get_fuzz_result(void *instrumentation_state) {
	afl_state_t * state = (afl_state_t *)instrumentation_state;

	// If we haven't set the fuzz results, do that and return the result
	if(!state->fuzz_results_set)
		return finish_fuzz_round(state);

	// otherwise we can just return the result
	return state->last_fuzz_result;
}

/**
 * This function determines if the target process CRASHED, HUNG or EXITED
 * NORMALLY, cleans up the process, and checks to see if any new code was
 * executed.  The assumption is that if you are calling this function,
 * you're sick of waiting for the child, so if it is still executing by the
 * time we get here, we're calling it a HANG.  This is implemented as an
 * internal function so we can use it when the caller calls any of the post-
 * fuzzing functions, such as get_fuzz_result or is_new_path().
 *
 * @param state - The AFL specific state structure
 * @return - either FUZZ_NONE, FUZZ_HANG, FUZZ_CRASH, or -1 on error.
 */
static int finish_fuzz_round(afl_state_t *state) {
	int status, rc;

	// if our process is still running, then it was a hang
	if(!state->process_finished) {
		destroy_target_process(state);
		state->last_fuzz_result = FUZZ_HANG;
#ifdef __x86_64__
		simplify_trace((uint64_t*)state->trace_bits);
#else
		simplify_trace((uint32_t*)state->trace_bits);
#endif /* ^__x86_64__ */
		state->last_is_new_path = has_new_bits(state->virgin_tmout, state->trace_bits);
		DEBUG_MSG("Process hung, has_new_bits = %d", state->last_is_new_path);
		state->fuzz_results_set = 1;
	} else if(WIFEXITED(state->last_status)) {
		state->last_fuzz_result = FUZZ_NONE;  // process exited normally
		state->last_is_new_path = has_new_bits(state->virgin_bits, state->trace_bits);
		DEBUG_MSG("Process exited normally, has_new_bits = %d", state->last_is_new_path);
		state->fuzz_results_set = 1;
	} else if(WIFSIGNALED(state->last_status)) {
		// process was terminated by a signal, we don't really care which one...
		state->last_fuzz_result = FUZZ_CRASH;
#ifdef __x86_64__
		simplify_trace((uint64_t*)state->trace_bits);
#else
		simplify_trace((uint32_t*)state->trace_bits);
#endif /* ^__x86_64__ */
		state->last_is_new_path = has_new_bits(state->virgin_crash, state->trace_bits);
		DEBUG_MSG("Process crashed, has_new_bits = %d", state->last_is_new_path);
		state->fuzz_results_set = 1;
	} else {
		// if it didn't exit normally, nor get interrupted by a signal...
		// I'm not sure what happened!
		return FUZZ_ERROR;
	}
	return state->last_fuzz_result;
}

/**
 * Checks if the target process is done fuzzing the inputs yet.
 * @param instrumentation_state - The afl_state_t object containing this
 *                                instrumentation's state
 * @return - 0 if the process is not done testing the fuzzed input,
 *           non-zero if the process is done.
 */
int afl_is_process_done(void *instrumentation_state) {
	int status, rc;
	afl_state_t * state = (afl_state_t *)instrumentation_state;

	// If the state says we're done, our job is easy!
	if(state->process_finished)
		return 1;

	if(state->use_fork_server) {
		status = fork_server_get_status(&state->fs, 0);
		// if it's still alive or an error occurred and we can't tell
		if(status < 0 || status == FORKSERVER_NO_RESULTS_READY)
			return 0;
		state->last_status = status;
		state->process_finished = 1;
		return 1;
	} else {
		// We just need to check to see if the process is still alive
		DEBUG_MSG("Calling waitpid() to see if we're done...");
		rc = waitpid(state->child_pid, &status, WNOHANG);
		DEBUG_MSG("status=%d ; waitpid() returned: %d", status, rc);
		if(rc == 0)  // child did not change state
			return 0;
		if(rc == state->child_pid) {
			// our child changed state (exited, received a signal, etc.)
			state->last_status = status;  // Record it
			state->child_pid = 0;         // We no longer have a child process
			state->process_finished = 1;  // Mark that we're done
			return 1;
		}
		if(rc == -1) // waitpid failed
			return -1;
		ERROR_MSG("waitpid() said pid %d changed state but our child was %d",
			rc, state->child_pid);
		return -1;  // Some other child process changed state?
	}

	ERROR_MSG("Fell through to end of afl_is_process_done().");
	return -1;
}

int afl_help(char **help_str) {
	*help_str = strdup(
		"afl - AFL-based instrumentation\n"
		"Options:\n"
		"  use_fork_server      Whether to use a fork server; 1=yes, 0=no (default=1)\n"
//		"  persistence_max_cnt  The number of executions to run in one process while\n"
//		"                         fuzzing in persistence mode (default=1)\n"
//		"  qemu_mode            Whether to use qemu mode; 1=yes, 0=no (default=0)\n"
		"\n"
	);
	if (*help_str == NULL)
		return -1;
	return 0;
}

/**
 * This function creates a linux_ipt_state_t object based on the given options.
 * @param options - A JSON string of the options to set in the new
 *                  afl_state_t. See the help function for more information on
 *                  the specific options available.
 * @return the afl_state_t generated from the options in the JSON options
 *         string, or NULL on failure
 */
static afl_state_t * setup_options(char *options) {
	afl_state_t * state;

	state = malloc(sizeof(afl_state_t));
	if(!state)
		return NULL;
	memset(state, 0, sizeof(afl_state_t));
	state->use_fork_server = 1;  // default to use the fork server

	if(options) {
		DEBUG_MSG("JSON options = %s", options);
		PARSE_OPTION_INT(state, options, use_fork_server,
				"use_fork_server", afl_cleanup);
//		PARSE_OPTION_INT(state, options, persistence_max_cnt,
//				"persistence_max_cnt", afl_cleanup);
	}

	return state;
}

/**
 * This function starts the fuzzed process
 * @param state - The afl_state_t object containing this instrumentation's state
 * @param cmd_line - the command line of the fuzzed process to start
 * @param input - a buffer to the input that should be sent to the fuzzed process
 * @param input_length - the length of the input parameter
 * @return - zero on success, non-zero on failure.
 */
static int create_target_process(afl_state_t * state, char* cmd_line,
			char * input, size_t input_length) {
	char ** argv;
	int i;

	DEBUG_MSG("Splitting command line arguments...");
	if(split_command_line(cmd_line, &state->target_path, &argv))
		return -1;
	if(state->use_fork_server) {
		DEBUG_MSG("Using fork server...");
		if(!state->fork_server_setup) {
			// Set up shared memory before starting the fork server
			if(setup_shm(state))
				return -1;
			fork_server_init(&state->fs, state->target_path, argv, 1,
					state->persistence_max_cnt, input_length != 0);
			state->fork_server_setup = 1;
		}
		if(fork_server_run(&state->fs))
			return -1;
	} else {
		// Set up shared memory before executing the target program
		if(setup_shm(state))
			return -1;
		DEBUG_MSG("Not using fork server, executing %s", state->target_path);
		state->child_pid = run_target(input_length != 0,
		                              state->target_path, argv,
		                              NULL, 0, NULL, NULL, 0);
		DEBUG_MSG("Child process ID = %d", state->child_pid);
	}

	if(state->use_fork_server) {
		state->child_pid = fork_server_fork(&state->fs);
		if(state->child_pid < 0)
			return -1;
	}

	for(i = 0; argv[i]; i++)
		free(argv[i]);
	free(argv);

	return 0;
}

/**
 * This function terminates the fuzzed process.
 * @param state - The afl_state_t object containing this instrumentation's state
 */
static void destroy_target_process(afl_state_t * state) {
	if(state->child_pid) {
		DEBUG_MSG("Cleaning up old child process (pid=%d)", state->child_pid);
		if(!state->persistence_max_cnt) {
			kill(state->child_pid, SIGKILL);
			state->child_pid = 0;
		}
		if(state->use_fork_server) {
			state->last_status = fork_server_get_status(&state->fs, 1);
		}
	}
}

/**
 * This sets up the shared memory between our fuzzer and the target process
 * being fuzzed.  The target process will write to this as it executes and
 * we will read it once the fuzzing is complete (crash, hang, or normal exit)
 * @param instrumentation_state - The afl_state_t object containing this
 *                                instrumentation's state
 * @returns zero on success, non-zero on error
 */
int setup_shm(void *instrumentation_state) {
	/*
	This function is based on the AFL setup_shm function present in afl-fuzz.c,
	available at this URL:
	https://github.com/mirrorer/afl/blob/master/afl-fuzz.c#L1968.
	AFL's license is as shown below:

	american fuzzy lop - fuzzer code
	--------------------------------
	Written and maintained by Michal Zalewski <lcamtuf@google.com>

	Forkserver design by Jann Horn <jannhorn@googlemail.com>

	Copyright 2013, 2014, 2015, 2016, 2017 Google Inc. All rights reserved.

	Licensed under the Apache License, Version 2.0 (the "License");
	you may not use this file except in compliance with the License.
	You may obtain a copy of the License at:

	http://www.apache.org/licenses/LICENSE-2.0
	*/
 
	char* shm_str;
	afl_state_t * state = (afl_state_t *)instrumentation_state;

	if(state->trace_bits) // if trace_bits already points at the shm
		return 0;     // region, we've already run this function!

	// If we loaded a saved input bitmap, do not overwrite the
	// map showing what was fuzzed showing everything as untouched
	if(!state->input_bitmap)
		memset(state->virgin_bits, 255, MAP_SIZE);

	memset(state->virgin_tmout, 255, MAP_SIZE);
	memset(state->virgin_crash, 255, MAP_SIZE);

	// Allocate shared memory; shm_id must be module level or global so
	// the atexit function has access to it (as we can not pass arguments
	// to the callback function)
	shm_id = shmget(IPC_PRIVATE, MAP_SIZE, IPC_CREAT | IPC_EXCL | 0600);
	if(shm_id < 0) {
		ERROR_MSG("shmget() failed");
		return 1;
	}
	atexit(remove_shm);
	shm_str = alloc_printf("%d", shm_id);

	// set the environment variable so the instrumented binary knows which
	// shared memory ID to attach to when it goes to write the bitmap
	setenv(SHM_ENV_VAR, shm_str, 1);
	ck_free(shm_str);

	// Attach to shared memory region
	state->trace_bits = shmat(shm_id, NULL, 0);
	if(!state->trace_bits) {
		ERROR_MSG("shmat() failed");
		return 1;
	}

	return 0;
}

/* Get rid of shared memory (atexit handler). */
static void remove_shm() {
	shmctl(shm_id, IPC_RMID, NULL);
}

/**
 * Check if the current execution path brings anything new to the table.
 * Update virgin bits to reflect the new paths found, so subsequent calls will
 * always return 0.
 *
 * This function is called after every exec() on a fairly large buffer, so
 * it needs to be fast. We do this in 32-bit and 64-bit flavors.
 *
 * @param virgin_map - The map we should compare against, which will be
 *                     virgin_{bits,tmout,crash} in practice.
 * @param trace_bits - The trace for this particular run
 * @returns - 1 if the only change is the hit-count for a particular tuple;
 *            2 if there are new tuples seen, 0 if it is not a new path
 **/
static inline uint8_t has_new_bits(uint8_t* virgin_map, uint8_t *trace_bits) {
	/*
	This function is based on the AFL has_new_bits function present in afl-fuzz.c,
	available at this URL:
	https://github.com/mirrorer/afl/blob/master/afl-fuzz.c#L1968.
	AFL's license is as shown below:

	american fuzzy lop - fuzzer code
	--------------------------------
	Written and maintained by Michal Zalewski <lcamtuf@google.com>

	Forkserver design by Jann Horn <jannhorn@googlemail.com>

	Copyright 2013, 2014, 2015, 2016, 2017 Google Inc. All rights reserved.

	Licensed under the Apache License, Version 2.0 (the "License");
	you may not use this file except in compliance with the License.
	You may obtain a copy of the License at:

	http://www.apache.org/licenses/LICENSE-2.0
	*/

#ifdef __x86_64__
  uint64_t* current = (uint64_t*)trace_bits;
  uint64_t* virgin  = (uint64_t*)virgin_map;
  uint32_t  i = (MAP_SIZE >> 3);
#else
  uint32_t* current = (uint32_t*)trace_bits;
  uint32_t* virgin  = (uint32_t*)virgin_map;
  uint32_t  i = (MAP_SIZE >> 2);
#endif /* ^__x86_64__ */

  uint8_t   ret = 0;
  while (i--) {
    /* Optimize for (*current & *virgin) == 0 - i.e., no bits in current bitmap
       that have not been already cleared from the virgin map - since this will
       almost always be the case. */
    if (unlikely(*current) && unlikely(*current & *virgin)) {
      if (likely(ret < 2)) {
        uint8_t* cur = (uint8_t*)current;
        uint8_t* vir = (uint8_t*)virgin;

        /* Looks like we have not found any new bytes yet; see if any non-zero
           bytes in current[] are pristine in virgin[]. */
#ifdef __x86_64__
        if ((cur[0] && vir[0] == 0xff) || (cur[1] && vir[1] == 0xff) ||
            (cur[2] && vir[2] == 0xff) || (cur[3] && vir[3] == 0xff) ||
            (cur[4] && vir[4] == 0xff) || (cur[5] && vir[5] == 0xff) ||
            (cur[6] && vir[6] == 0xff) || (cur[7] && vir[7] == 0xff)) ret = 2;
        else ret = 1;
#else
        if ((cur[0] && vir[0] == 0xff) || (cur[1] && vir[1] == 0xff) ||
            (cur[2] && vir[2] == 0xff) || (cur[3] && vir[3] == 0xff)) ret = 2;
        else ret = 1;
#endif /* ^__x86_64__ */
      }
      *virgin &= ~*current;
    }
    current++;
    virgin++;
  }
  return ret;
}

#ifdef __x86_64__
static void simplify_trace(uint64_t* mem) {
  uint32_t i = MAP_SIZE >> 3;
  while (i--) {
    /* Optimize for sparse bitmaps. */
    if (unlikely(*mem)) {
      uint8_t* mem8 = (uint8_t*)mem;
      mem8[0] = simplify_lookup[mem8[0]];
      mem8[1] = simplify_lookup[mem8[1]];
      mem8[2] = simplify_lookup[mem8[2]];
      mem8[3] = simplify_lookup[mem8[3]];
      mem8[4] = simplify_lookup[mem8[4]];
      mem8[5] = simplify_lookup[mem8[5]];
      mem8[6] = simplify_lookup[mem8[6]];
      mem8[7] = simplify_lookup[mem8[7]];
    } else *mem = 0x0101010101010101ULL;
    mem++;
  }
}
#else
static void simplify_trace(uint32_t* mem) {
  uint32_t i = MAP_SIZE >> 2;
  while (i--) {
    /* Optimize for sparse bitmaps. */
    if (unlikely(*mem)) {
      uint8_t* mem8 = (uint8_t*)mem;
      mem8[0] = simplify_lookup[mem8[0]];
      mem8[1] = simplify_lookup[mem8[1]];
      mem8[2] = simplify_lookup[mem8[2]];
      mem8[3] = simplify_lookup[mem8[3]];
    } else *mem = 0x01010101;
    mem++;
  }
}
#endif /* ^__x86_64__ */
