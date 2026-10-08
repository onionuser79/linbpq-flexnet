// Replaces upstream stdexcept.c (and Win32bits/StdExcept.c) in the mingw-w64
// build: the original dumps the stack with MSVC __asm. Under winshim.h this
// handler is dead code.
// Same contract as upstream: no closing brace, the including code supplies it.
__except(EXCEPTION_EXECUTE_HANDLER)
{
	Debugprintf("BPQ32 *** Program Error in %s", EXCEPTMSG);

#undef EXCEPTMSG
