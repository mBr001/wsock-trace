-- wsock_trace_init.lua
--

function os_details()
  return string.format ("%s on %s (%s)",
                        jit.version, jit.os, jit.arch);
end

function __FILE__()
  local src = debug.getinfo(2,'S').source
  return string.sub (src,2)
end

who_am_I = __FILE__()

io.write (string.format("Hello from '%s': %s\n", who_am_I, os_details()))
io.write ("package.path[]: " .. package.path .. '.\n')
