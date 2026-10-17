// Clawd Mochi for OpenCode v2: the desk toy reacts to OpenCode the way it does to Claude Code.
// Thinking while a reply runs, working (with the tool name) during tool calls, flashing when a
// permission is asked, "done!" when the session goes idle. Talks through `clawd-mochi <command>`,
// which is a no-op while the Mochi is unplugged.
import { spawn } from "node:child_process"

const send = (line) =>
  spawn("clawd-mochi", [line], { stdio: "ignore" }).on("error", () => {})

export default {
  id: "clawd-mochi",
  setup: async (ctx) => {
    await ctx.tool.hook("execute.before", (call) => {
      send(`mood work ${String(call?.tool ?? "tool")}`)
    })

    let stopped = false
    ;(async () => {
      for await (const ev of ctx.event.subscribe()) {
        if (stopped) break
        switch (ev.type) {
          case "session.execution.started":
          case "session.tool.success":
          case "session.tool.failed":
          case "permission.replied":
            send("mood think")
            break
          case "permission.asked":
            send("mood wait allow it?")
            break
          case "session.execution.failed":
          case "session.error":
            send("mood wait something broke")
            break
          case "session.idle":
            send("mood happy done!")
            break
        }
      }
    })().catch(() => {})
    return () => {
      stopped = true
    }
  },
}
