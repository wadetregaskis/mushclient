-- tests/win/app_probes/harness.lua -- shared by every in-app probe.
--
-- These probes run inside the REAL GUI app, which is the only place a
-- CMUSHView, a CSendView, a CMainFrame and real GDI all exist at once.  The
-- unit target (MUSHclient_tests.vcxproj) constructs a CMUSHclientDoc with no
-- window and can reach none of it; these are the Windows counterpart of the
-- macOS in-app selftests.
--
-- Each probe writes "PASS <name>" / "FAIL <name>: ..." lines and one SUMMARY
-- line.  run_app_probes.ps1 counts them, and treats a missing SUMMARY as a
-- crash -- so a probe that dies half way is a failure, not a silent pass.
--
-- Every assertion must be written so that "the code did nothing" FAILS.  A
-- getter that returns the value you just set proves nothing if the setter is
-- also inert; assert against something independently known.

local M = { passed = 0, failed = 0, out = nil }

function M.open (name)
  -- Set by the generated bootstrap (run_app_probes.ps1), not by the
  -- environment: the scripting sandbox's os table is not something to depend on.
  local dir = assert (MC_PROBE_OUT, "MC_PROBE_OUT not set")
  M.out = assert (io.open (dir .. "/" .. name .. ".txt", "w"))
  M.name = name
end

-- NB: `f:write (...)` truncates a vararg to its FIRST value in Lua, which once
-- produced a report file of bare labels that looked like a clean run.
function M.say (...)
  local t, n = {...}, select ("#", ...)
  for i = 1, n do M.out:write (tostring (t [i])) end
  M.out:write ("\n")
end

function M.hex (v)
  return type (v) == "number" and string.format ("0x%06X", v) or tostring (v)
end

function M.ok (cond, name, detail)
  if cond then
    M.passed = M.passed + 1
    M.say ("PASS ", name)
  else
    M.failed = M.failed + 1
    M.say ("FAIL ", name, ": ", tostring (detail or ""))
  end
end

function M.eq (got, want, name)
  M.ok (got == want, name, "expected " .. tostring (want) .. ", got " .. tostring (got))
end

function M.eqhex (got, want, name)
  M.ok (got == want, name, "expected " .. M.hex (want) .. ", got " .. M.hex (got))
end

function M.neq (got, unwanted, name)
  M.ok (got ~= unwanted, name, "expected anything but " .. tostring (unwanted))
end

function M.finish ()
  M.say ("SUMMARY ", M.passed, " passed, ", M.failed, " failed")
  M.out:close ()
end

-- Run body under pcall so a Lua error still produces a SUMMARY naming it,
-- rather than a truncated file the runner has to guess about.
function M.run (name, body)
  M.open (name)
  local ok, err = pcall (body)
  if not ok then
    M.failed = M.failed + 1
    M.say ("FAIL ", name, " raised: ", tostring (err))
  end
  M.finish ()
end

return M
