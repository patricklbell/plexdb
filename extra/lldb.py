import lldb


def async_bt_command(debugger, command, exe_ctx, result, internal_dict):
    """Print the plexdb coroutine async callstack (PLEXDB_DEBUG=1 builds only).
    Calls plexdb_async_stack() which walks g_current_frame for the selected thread."""
    frame = exe_ctx.GetFrame()
    if not frame.IsValid():
        result.AppendMessage("<no selected frame>")
        return
    frame.EvaluateExpression("(void)plexdb_async_stack()")


def mem_hex_command(debugger, command, exe_ctx, result, internal_dict):
    # Expected usage:
    # memhex <address> <count>

    args = command.split()
    if len(args) != 2:
        result.AppendMessage("Usage: memhex <address> <count>")
        return

    addr = int(args[0], 0)
    count = int(args[1], 0)

    target = debugger.GetSelectedTarget()
    process = target.GetProcess()

    error = lldb.SBError()
    data = process.ReadMemory(addr, count, error)

    if not error.Success():
        result.AppendMessage("<error reading memory>")
        return

    value = 0
    for i, b in enumerate(data):
        value |= b << (8 * i)

    result.AppendMessage("0x{:x}".format(value))


def __lldb_init_module(debugger, internal_dict):
    debugger.HandleCommand("command script add -f lldb.async_bt_command async-bt")
    debugger.HandleCommand("command script add -f lldb.mem_hex_command memhex")
