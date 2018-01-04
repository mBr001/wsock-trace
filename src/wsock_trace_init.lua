-- wsock_trace_init.lua
--

function os_details()
  return string.format ("%s on %s (%s)", jit.version, jit.os, jit.arch);
end

function __FILE__()
  local src = debug.getinfo(2,'S').source
  return string.sub (src,2)
end

function __LINE__()
  return debug.getinfo(2,'l').currentline
end

function __FUNC__()
  return debug.getinfo(2,'n').name
end

WSAStartup = function (arg)
  io.write (string.format("Hello from %s().\n", __FUNC__()))
end

--- Try the MinGW base names first.

--- if jit.arch == "x64" then
---   ws_name = "wsock_trace_mw_x64"
--- else
---   ws_name = "wsock_trace_mw"
--- end
---
--- if not package.loaded [ws_name] then
---   ws = require (ws_name)
--- else
---   --- Then if 'ws == nil', try the MSVC base names.
---
---   if jit.arch == "x64" then
---     ws_name = "wsock_trace_x64"
---   else
---     ws_name = "wsock_trace"
---   end
---   ws = require (ws_name)
--- end

if jit.arch == "x64" then
  ws_name = "wsock_trace_x64"
else
  ws_name = "wsock_trace"
end

if package.loaded [ws_name] then
  ws.trace_puts (string.format("  Package ~2%s~0 already loaded; ~1ws -> %p~0\n", ws_name, ws))
else
  ws = require (ws_name)
end

function trace_printf (fmt, ...)
  ws.trace_puts ("\ntrace_printf(): ")
  str = string.format (fmt, unpack(arg))
  ws.trace_puts (str)
end

who_am_I = __FILE__()

ws.trace_puts (string.format("  ws.get_trace_level: ~1%d~0.\n", ws.get_trace_level()))

if nil then
  ws.trace_puts ("  ws.set_trace_level(0).\n")
  ws.set_trace_level (0)
end

-- trace_printf ("Hello from: ~1%s~0.\n", who_am_I)

-- ws.trace_printf ("Hello from: ~1%s~0.\nVersion:  ~1%s~0. Arg: %s, another arg: %s\n", who_am_I, os_details(), "10", "hello")
-- ws.trace_printf ("Hello from: ~1%s~0.\n", who_am_I)

ws.trace_puts (string.format("  Hello from:         ~1%s~0.\n", who_am_I))
ws.trace_puts (string.format("  Version:            ~1%s~0.\n", os_details()))
ws.trace_puts (string.format("  This is line:       ~1%d~0.\n", __LINE__()))

ws.trace_puts ("  package.path[]:     ~1" .. package.path .. "~0.\n")
ws.trace_puts ("  package.cpath[]:    ~1" .. package.cpath .. "~0.\n")

ws.trace_puts (string.format("  I'm importing from: ~1%s~0. ws.get_builder(): ~1%s~0.\n",
               ws.get_dll_full_name(), ws.get_builder()))

ws.register_hook (WSAStartup, "2")
WSAStartup()
