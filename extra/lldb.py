import lldb


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
