#include "qemu/osdep.h"
#include "qemu/main-loop.h"

static bool bql_is_locked = false;

bool bql_locked(void) { return bql_is_locked; }

void bql_lock_impl(const char* file, int line) { }

void bql_unlock(void) { }

void bql_lockless_section_begin(void) { }

void bql_lockless_section_end(void) { }
