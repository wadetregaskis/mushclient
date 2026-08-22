-- clipboard_arg 10 names the WHOLE MATCHING LINE, end to end.
--
-- tests/win/test_gap_coverage.cpp asserts ClipboardWildcardIndex's mapping, but
-- deliberately not the call site: driving it puts text on the machine's real
-- clipboard.  Here that is affordable, because world.GetClipboard /
-- SetClipboard let the probe save and restore what was there.
--
-- docs/UPSTREAM-BUGS.md 109.

local H = dofile (assert (MC_PROBE_DIR) .. "/harness.lua")

function probeMain ()
 H.run ("clipboard", function ()

  local saved = world.GetClipboard ()          -- restored at the end, always
  local function restore () world.SetClipboard (saved or "") end

  local ok, err = pcall (function ()

    -- A trigger with three capture groups, so %1..%3 and the whole line all
    -- differ and no two assertions can pass for the same reason.
    local function armed (arg)
      world.DeleteTrigger ("cb")
      -- eEnabled (0x01) + eTriggerRegularExpression (0x20) + eReplace (0x400).
      -- Flags 0 would add it DISABLED, and a disabled trigger never matches,
      -- which would make every assertion below pass for the wrong reason.
      H.eq (world.AddTriggerEx ("cb", [[^GOT (\w+) (\w+) (\w+)$]], "",
                                0x01 + 0x20 + 0x400, -1, 0, "", "", 0, 100), 0,
            "AddTriggerEx arg=" .. arg)
      H.eq (world.SetTriggerOption ("cb", "clipboard_arg", tostring (arg)), 0,
            "SetTriggerOption clipboard_arg=" .. arg)
      world.SetClipboard ("SENTINEL")          -- so "did nothing" is visible
      world.Simulate ("GOT alpha beta gamma\n")
      return world.GetClipboard ()
    end

    H.eq (armed (1), "alpha", "clipboard_arg 1 copies %1")
    H.eq (armed (3), "gamma", "clipboard_arg 3 copies %3")

    -- The fix.  Before it, 10 failed the `< MAX_WILDCARDS` bound and the
    -- clipboard kept SENTINEL.
    H.eq (armed (10), "GOT alpha beta gamma", "clipboard_arg 10 copies the whole line")

    -- 0 is "(none)" and must copy nothing -- the control that stops the three
    -- above from passing on a build where every match copies something.
    H.eq (armed (0), "SENTINEL", "clipboard_arg 0 copies nothing")

    world.DeleteTrigger ("cb")
  end)

  restore ()
  if not ok then error (err, 0) end
 end)
end
