/*
===========================================================================

Doom 3 GPL Source Code
Copyright (C) 1999-2011 id Software LLC, a ZeniMax Media company.

This file is part of the Doom 3 GPL Source Code ("Doom 3 Source Code").

Doom 3 Source Code is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

Doom 3 Source Code is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Doom 3 Source Code.  If not, see <http://www.gnu.org/licenses/>.

In addition, the Doom 3 Source Code is also subject to certain additional terms. You should have received a copy of these additional terms immediately following the terms and conditions of the GNU General Public License which accompanied the Doom 3 Source Code.  If not, please request a copy in writing from id Software at the address below.

If you have questions concerning this license or the applicable additional terms, you may contact in writing id Software LLC, c/o ZeniMax Media Inc., Suite 120, Rockville, Maryland 20850 USA.

===========================================================================
*/

#include "sys/sys_sdl.h"

#if 0 // TODO: was there a reason not to include full SDL.h?
  #include <SDL_version.h>
  #include <SDL_mutex.h>
  #include <SDL_thread.h>
  #include <SDL_timer.h>
#endif

#include "sys/platform.h"
#include "framework/Common.h"

#include "sys/sys_public.h"

#ifdef BAREMETAL
extern "C" {
#include "engine/dos_thread.h"
}
#endif

#if SDL_MAJOR_VERSION < 2
  // SDL1.2 doesn't have SDL_threadID but uses Uint32.
  // this typedef helps using the same code for SDL1.2 and SDL2
  typedef Uint32 SDL_threadID;
#elif SDL_MAJOR_VERSION >= 3
  // backwards-compat with SDL2
  #define SDL_mutex SDL_Mutex
  #define SDL_cond SDL_Condition
  #define SDL_threadID SDL_ThreadID
  #define SDL_CreateCond SDL_CreateCondition
  #define SDL_DestroyCond SDL_DestroyCondition
  #define SDL_CondWait SDL_WaitCondition
  #define SDL_CondSignal SDL_SignalCondition
#endif

#if SDL_MAJOR_VERSION < 3
  // in SDL3 SDL_ThreadID is the type (that's called SDL_threadID with lowercase-t in SDL2),
  // in SDL1.2 and SDL2 SDL_ThreadID() is a function that returns the current thread's ID...
  // So use SDL_GetCurrentThreadID() in all cases to avoid this clash
  #define SDL_GetCurrentThreadID SDL_ThreadID
#endif

#if __cplusplus >= 201103
  // xthreadinfo::threadId doesn't use SDL_threadID directly so we don't drag SDL headers into sys_public.h
  // but we should still make sure that the type fits (in SDL1.2 it's Uint32, in SDL2 it's unsigned long)
  static_assert( sizeof(SDL_threadID) <= sizeof(xthreadInfo::threadId), "xthreadInfo::threadId has unsuitable type!" );
#endif

static SDL_mutex	*mutex[MAX_CRITICAL_SECTIONS] = { };
static SDL_cond		*cond[MAX_TRIGGER_EVENTS] = { };
static bool			signaled[MAX_TRIGGER_EVENTS] = { };
static bool			waiting[MAX_TRIGGER_EVENTS] = { };

static xthreadInfo	*thread[MAX_THREADS] = { };
static size_t		thread_count = 0;

static bool mainThreadIDset = false;
static SDL_threadID mainThreadID = -1;

#ifdef BAREMETAL
struct bmCriticalSection {
	volatile int locked;
	volatile unsigned long owner;
	int depth;
};

static const unsigned long BM_NO_OWNER = ~0UL;
static bmCriticalSection bm_lock[MAX_CRITICAL_SECTIONS];
static bool bm_asyncThreadReal = false;

static ID_INLINE void BM_CpuPauseThread() {
	__asm__ __volatile__(".insn i 0x0F, 0, x0, x0, 0x010" ::: "memory");
}

static bool BM_IsAsyncThreadName( const char *name ) {
	const char *want = "AsyncThread";
	if ( name == NULL ) {
		return false;
	}
	while ( *name != '\0' && *want != '\0' && *name == *want ) {
		name++;
		want++;
	}
	return *name == '\0' && *want == '\0';
}

extern "C" bool Sys_BareMetalAsyncThreadIsReal( void ) {
	return bm_asyncThreadReal;
}
#endif

/*
==============
Sys_Sleep
==============
*/
void Sys_Sleep(int msec) {
	SDL_Delay(msec);
}

/*
==================
Sys_InitThreads
==================
*/
void Sys_InitThreads() {
#ifdef BAREMETAL
	dos_threads_init();
	mainThreadID = (SDL_threadID)dos_thread_self_id();
	mainThreadIDset = true;

	for (int i = 0; i < MAX_CRITICAL_SECTIONS; i++) {
		bm_lock[i].locked = 0;
		bm_lock[i].owner = BM_NO_OWNER;
		bm_lock[i].depth = 0;
		mutex[i] = NULL;
	}

	for (int i = 0; i < MAX_TRIGGER_EVENTS; i++) {
		cond[i] = NULL;
		signaled[i] = false;
		waiting[i] = false;
	}

	for (int i = 0; i < MAX_THREADS; i++)
		thread[i] = NULL;

	thread_count = 0;
	bm_asyncThreadReal = false;
	return;
#else
	mainThreadID = SDL_GetCurrentThreadID();
	mainThreadIDset = true;

	// critical sections
	for (int i = 0; i < MAX_CRITICAL_SECTIONS; i++) {
		mutex[i] = SDL_CreateMutex();

		if (!mutex[i]) {
			Sys_Printf("ERROR: SDL_CreateMutex failed\n");
			return;
		}
	}

	// events
	for (int i = 0; i < MAX_TRIGGER_EVENTS; i++) {
		cond[i] = SDL_CreateCond();

		if (!cond[i]) {
			Sys_Printf("ERROR: SDL_CreateCond failed\n");
			return;
		}

		signaled[i] = false;
		waiting[i] = false;
	}

	// threads
	for (int i = 0; i < MAX_THREADS; i++)
		thread[i] = NULL;

	thread_count = 0;
#endif
}

/*
==================
Sys_ShutdownThreads
==================
*/
void Sys_ShutdownThreads() {
	// threads
	for (int i = 0; i < MAX_THREADS; i++) {
		if (!thread[i])
			continue;

		Sys_Printf("WARNING: Thread '%s' still running\n", thread[i]->name);
#ifdef BAREMETAL
		if (dos_thread_is_handle((void *)thread[i]->threadHandle)) {
			dos_thread_join((void *)thread[i]->threadHandle);
		}
#elif SDL_VERSION_ATLEAST(2, 0, 0)
		// TODO no equivalent in SDL2
#else
		SDL_KillThread(thread[i]->threadHandle);
#endif
		thread[i] = NULL;
	}

	// events
	for (int i = 0; i < MAX_TRIGGER_EVENTS; i++) {
#ifndef BAREMETAL
		SDL_DestroyCond(cond[i]);
#endif
		cond[i] = NULL;
		signaled[i] = false;
		waiting[i] = false;
	}

	// critical sections
	for (int i = 0; i < MAX_CRITICAL_SECTIONS; i++) {
#ifndef BAREMETAL
		SDL_DestroyMutex(mutex[i]);
#endif
		mutex[i] = NULL;
#ifdef BAREMETAL
		bm_lock[i].locked = 0;
		bm_lock[i].owner = BM_NO_OWNER;
		bm_lock[i].depth = 0;
#endif
	}
#ifdef BAREMETAL
	bm_asyncThreadReal = false;
#endif
}

/*
==================
Sys_EnterCriticalSection
==================
*/
void Sys_EnterCriticalSection(int index) {
	assert(index >= 0 && index < MAX_CRITICAL_SECTIONS);

#ifdef BAREMETAL
	unsigned long self = dos_thread_self_id();
	if ( bm_lock[index].owner == self ) {
		bm_lock[index].depth++;
		return;
	}
	while ( __sync_lock_test_and_set( &bm_lock[index].locked, 1 ) != 0 ) {
		BM_CpuPauseThread();
	}
	__sync_synchronize();
	bm_lock[index].owner = self;
	bm_lock[index].depth = 1;
#else
#if SDL_VERSION_ATLEAST(3, 0, 0)
	SDL_LockMutex(mutex[index]); // in SDL3, this returns void and can't fail
#else // SDL2 and SDL1.2
	if (SDL_LockMutex(mutex[index]) != 0)
		common->Error("ERROR: SDL_LockMutex failed\n");
#endif
#endif
}

/*
==================
Sys_LeaveCriticalSection
==================
*/
void Sys_LeaveCriticalSection(int index) {
	assert(index >= 0 && index < MAX_CRITICAL_SECTIONS);

#ifdef BAREMETAL
	unsigned long self = dos_thread_self_id();
	assert(bm_lock[index].owner == self);
	if ( --bm_lock[index].depth > 0 ) {
		return;
	}
	bm_lock[index].owner = BM_NO_OWNER;
	__sync_synchronize();
	__sync_lock_release( &bm_lock[index].locked );
#else
#if SDL_VERSION_ATLEAST(3, 0, 0)
	SDL_UnlockMutex(mutex[index]); // in SDL3, this returns void and can't fail
#else // SDL2 and SDL1.2
	if (SDL_UnlockMutex(mutex[index]) != 0)
		common->Error("ERROR: SDL_UnlockMutex failed\n");
#endif
#endif
}

/*
======================================================
wait and trigger events
we use a single lock to manipulate the conditions, CRITICAL_SECTION_SYS

the semantics match the win32 version. signals raised while no one is waiting stay raised until a wait happens (which then does a simple pass-through)

NOTE: we use the same mutex for all the events. I don't think this would become much of a problem
cond_wait unlocks atomically with setting the wait condition, and locks it back before exiting the function
the potential for time wasting lock waits is very low
======================================================
*/

/*
==================
Sys_WaitForEvent
==================
*/
void Sys_WaitForEvent(int index) {
	assert(index >= 0 && index < MAX_TRIGGER_EVENTS);

#ifdef BAREMETAL
	Sys_EnterCriticalSection(CRITICAL_SECTION_SYS);

	assert(!waiting[index]);
	if (__atomic_load_n(&signaled[index], __ATOMIC_ACQUIRE)) {
		__atomic_store_n(&signaled[index], false, __ATOMIC_RELEASE);
		Sys_LeaveCriticalSection(CRITICAL_SECTION_SYS);
		return;
	}

	waiting[index] = true;
	Sys_LeaveCriticalSection(CRITICAL_SECTION_SYS);

	while (!__atomic_load_n(&signaled[index], __ATOMIC_ACQUIRE)) {
		BM_CpuPauseThread();
	}

	Sys_EnterCriticalSection(CRITICAL_SECTION_SYS);
	__atomic_store_n(&signaled[index], false, __ATOMIC_RELEASE);
	waiting[index] = false;
	Sys_LeaveCriticalSection(CRITICAL_SECTION_SYS);
#else
	Sys_EnterCriticalSection(CRITICAL_SECTION_SYS);

	assert(!waiting[index]);	// WaitForEvent from multiple threads? that wouldn't be good
	if (signaled[index]) {
		// emulate windows behaviour: signal has been raised already. clear and keep going
		signaled[index] = false;
	} else {
		waiting[index] = true;
#if SDL_VERSION_ATLEAST(3, 0, 0)
		SDL_CondWait(cond[index], mutex[CRITICAL_SECTION_SYS]); // in SDL3, this returns void and can't fail
#else // SDL2 and SDL1.2
		if (SDL_CondWait(cond[index], mutex[CRITICAL_SECTION_SYS]) != 0)
			common->Error("ERROR: SDL_CondWait failed\n");
#endif
		waiting[index] = false;
	}

	Sys_LeaveCriticalSection(CRITICAL_SECTION_SYS);
#endif
}

/*
==================
Sys_TriggerEvent
==================
*/
void Sys_TriggerEvent(int index) {
	assert(index >= 0 && index < MAX_TRIGGER_EVENTS);

	Sys_EnterCriticalSection(CRITICAL_SECTION_SYS);

#ifdef BAREMETAL
	__atomic_store_n(&signaled[index], true, __ATOMIC_RELEASE);
#else
	if (waiting[index]) {
#if SDL_VERSION_ATLEAST(3, 0, 0)
		SDL_CondSignal(cond[index]); // in SDL3, this returns void and can't fail
#else // SDL2 and SDL1.2
		if (SDL_CondSignal(cond[index]) != 0)
			common->Error("ERROR: SDL_CondSignal failed\n");
#endif
	} else {
		// emulate windows behaviour: if no thread is waiting, leave the signal on so next wait keeps going
		signaled[index] = true;
	}
#endif

	Sys_LeaveCriticalSection(CRITICAL_SECTION_SYS);
}

/*
==================
Sys_CreateThread
==================
*/
void Sys_CreateThread(xthread_t function, void *parms, xthreadInfo& info, const char *name) {
	Sys_EnterCriticalSection();

#ifdef BAREMETAL
	if ( BM_IsAsyncThreadName( name ) ) {
#ifndef BM_REAL_ASYNC_THREAD
		info.name = name;
		info.threadHandle = (SDL_Thread *)(void *)(uintptr_t)1;
		info.threadId = mainThreadIDset ? (uintptr_t)mainThreadID : 0;
		bm_asyncThreadReal = false;
		if (thread_count < MAX_THREADS)
			thread[thread_count++] = &info;
		Sys_LeaveCriticalSection();
		return;
#endif
	}

	void *dosThread = dos_thread_create(function, parms, name);
	info.name = name;
	info.threadHandle = (SDL_Thread *)(dosThread ? dosThread : (void *)(uintptr_t)1);
	info.threadId = dosThread ? dos_thread_hartid(dosThread) : 0;
	if ( BM_IsAsyncThreadName( name ) ) {
		bm_asyncThreadReal = dosThread != NULL;
		if ( !bm_asyncThreadReal ) {
			common->DPrintf("WARNING: dOS could not start real AsyncThread; using main-loop async pump\n");
		}
	}
	if (thread_count < MAX_THREADS)
		thread[thread_count++] = &info;
	Sys_LeaveCriticalSection();
	return;
#endif

#if SDL_VERSION_ATLEAST(2, 0, 0)
	SDL_Thread *t = SDL_CreateThread(function, name, parms);
#else
	SDL_Thread *t = SDL_CreateThread(function, parms);
#endif

	if (!t) {
		common->Error("ERROR: SDL_thread for '%s' failed\n", name);
		Sys_LeaveCriticalSection();
		return;
	}

	info.name = name;
	info.threadHandle = t;
	info.threadId = SDL_GetThreadID(t);

	if (thread_count < MAX_THREADS)
		thread[thread_count++] = &info;
	else
		common->DPrintf("WARNING: MAX_THREADS reached\n");

	Sys_LeaveCriticalSection();
}

/*
==================
Sys_DestroyThread
==================
*/
void Sys_DestroyThread(xthreadInfo& info) {
	assert(info.threadHandle);

#ifdef BAREMETAL
	if ( BM_IsAsyncThreadName( info.name ) ) {
		bm_asyncThreadReal = false;
	}
	if (dos_thread_is_handle((void *)info.threadHandle)) {
		dos_thread_join((void *)info.threadHandle);
	}
#else
	SDL_WaitThread(info.threadHandle, NULL);
#endif

	info.name = NULL;
	info.threadHandle = NULL;
	info.threadId = 0;

	Sys_EnterCriticalSection();

	for (int i = 0; i < thread_count; i++) {
		if (&info == thread[i]) {
			thread[i] = NULL;

			int j;
			for (j = i + 1; j < thread_count; j++)
				thread[j - 1] = thread[j];

			thread[j - 1] = NULL;
			thread_count--;

			break;
		}
	}

	Sys_LeaveCriticalSection( );
}

/*
==================
Sys_GetThreadName
find the name of the calling thread
==================
*/
const char *Sys_GetThreadName(int *index) {
	const char *name;

	Sys_EnterCriticalSection();

#ifdef BAREMETAL
	SDL_threadID id = (SDL_threadID)dos_thread_self_id();
#else
	SDL_threadID id = SDL_GetCurrentThreadID();
#endif

	for (int i = 0; i < thread_count; i++) {
		if (id == thread[i]->threadId) {
			if (index)
				*index = i;

			name = thread[i]->name;

			Sys_LeaveCriticalSection();

			return name;
		}
	}

	if (index)
		*index = -1;

	Sys_LeaveCriticalSection();

	return "main";
}


/*
==================
Sys_IsMainThread
returns true if the current thread is the main thread
==================
*/
bool Sys_IsMainThread() {
#ifdef BAREMETAL
	if ( mainThreadIDset )
		return dos_thread_self_id() == mainThreadID;
	return true;
#else
	if ( mainThreadIDset )
		return SDL_GetCurrentThreadID() == mainThreadID;
	// if this is called before mainThreadID is set, we haven't created
	// any threads yet so it should be the main thread
	return true;
#endif
}
