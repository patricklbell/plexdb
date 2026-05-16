# plexdb GDB helpers
# Usage: source extra/gdb.gdb

# async-bt: print the coroutine async callstack.
# Requires a PLEXDB_DEBUG=1 build.
define async-bt
  call (void)plexdb_async_stack()
end

document async-bt
Print the plexdb coroutine async callstack.
Calls plexdb_async_stack() which walks g_current_frame for the selected thread.
Requires a PLEXDB_DEBUG=1 build.
end
