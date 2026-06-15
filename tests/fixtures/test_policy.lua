-- Test policy: soft_watch writes, tag reads with source
function on_event(ev)
    if ev.op == "write" then
        return { verdict = "soft_watch", extra = { reason = "write_detected" } }
    end
    if ev.op == "read" then
        return { verdict = "allow", extra = { source = ev.backend } }
    end
    return { verdict = "allow" }
end
