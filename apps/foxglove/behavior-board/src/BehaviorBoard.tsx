// The planner's status board: its behavior tree with what each node returned in the last tick
// (the path that holds in green), and the lane change state machine (the active state in
// green), from ets2_msgs/BehaviorStatus on /ets2/planning/behavior.
import { MessageEvent, PanelExtensionContext } from "@foxglove/extension";
import { ReactElement, useEffect, useLayoutEffect, useState } from "react";
import { createRoot } from "react-dom/client";

const TOPIC = "/ets2/planning/behavior";
const STALE_MS = 1000;

type Status = {
  node_name: string[];
  node_kind: string[];
  node_parent: ArrayLike<number>;
  node_status: ArrayLike<number>;
  fsm_state: string[];
  fsm_active: number;
  fsm_note: string;
  decision: string;
  shadow: boolean;
};

type Palette = { text: string; muted: string; faint: string; active: string; alert: string; line: string; background: string };
const palettes: Record<"light" | "dark", Palette> = {
  light: { text: "#1d2127", muted: "#6b7480", faint: "#b5bcc5", active: "#14883e", alert: "#c0392b", line: "#d9dde2", background: "#ffffff" },
  dark: { text: "#e6e9ed", muted: "#8b95a1", faint: "#4a525c", active: "#3ccf6e", alert: "#ff6b5b", line: "#2c333b", background: "#15181c" },
};

// Box-drawing prefix per node from its ancestors: whether each ancestor (and the node) is the
// last child of its parent.
function prefixes(parent: ArrayLike<number>): string[] {
  const n = parent.length;
  const last: boolean[] = new Array<boolean>(n).fill(true);
  for (let i = 0; i < n; i++) {
    for (let j = i + 1; j < n; j++) {
      if (parent[j] === parent[i]) {
        last[i] = false;
        break;
      }
    }
  }
  const out: string[] = [];
  for (let i = 0; i < n; i++) {
    if ((parent[i] ?? -1) < 0) {
      out.push("");
      continue;
    }
    let lead = "";
    for (let a = parent[i] ?? -1; a >= 0 && (parent[a] ?? -1) >= 0; a = parent[a] ?? -1) {
      lead = (last[a] === true ? "   " : "│  ") + lead;
    }
    out.push(lead + (last[i] === true ? "└─ " : "├─ "));
  }
  return out;
}

const GLYPH = ["·", "●", "✓", "–"]; // not ticked, running, success, failure

function BehaviorBoard({ context }: { context: PanelExtensionContext }): ReactElement {
  const [status, setStatus] = useState<Status | undefined>();
  const [received, setReceived] = useState(0);
  const [now, setNow] = useState(Date.now());
  const [scheme, setScheme] = useState<"light" | "dark">("dark");
  const [renderDone, setRenderDone] = useState<(() => void) | undefined>();

  useLayoutEffect(() => {
    context.onRender = (renderState, done) => {
      setRenderDone(() => done);
      if (renderState.colorScheme) {
        setScheme(renderState.colorScheme);
      }
      const frame = renderState.currentFrame ?? [];
      let last: MessageEvent | undefined;
      for (const m of frame) {
        if (m.topic === TOPIC) {
          last = m;
        }
      }
      if (last != undefined) {
        setStatus(last.message as Status);
        setReceived(Date.now());
      }
    };
    context.watch("currentFrame");
    context.watch("colorScheme");
    context.subscribe([{ topic: TOPIC }]);
  }, [context]);

  useEffect(() => {
    renderDone?.();
  }, [renderDone]);

  // Staleness without new messages (the planner stopped, or playback paused).
  useEffect(() => {
    const timer = setInterval(() => {
      setNow(Date.now());
    }, 500);
    return () => {
      clearInterval(timer);
    };
  }, []);

  const p = palettes[scheme];
  const stale = status == undefined || now - received > STALE_MS;
  const base = { fontFamily: "ui-monospace, SFMono-Regular, Consolas, monospace", fontSize: 12, color: p.text, background: p.background };
  if (!status) {
    return <div style={{ ...base, padding: 12, color: p.muted, height: "100%" }}>{TOPIC} 기다리는 중</div>;
  }
  const emergency = status.decision.startsWith("emergency");
  const activeColor = emergency ? p.alert : p.active;
  const lead = prefixes(status.node_parent);
  return (
    <div style={{ ...base, padding: "10px 12px", height: "100%", overflow: "auto", opacity: stale ? 0.45 : 1 }}>
      <div style={{ display: "flex", alignItems: "baseline", gap: 8, marginBottom: 8 }}>
        <span style={{ fontSize: 14, fontWeight: 600, color: emergency ? p.alert : p.text }}>{status.decision || "—"}</span>
        {status.shadow && <span style={{ color: p.muted }}>shadow</span>}
        {stale && <span style={{ color: p.muted }}>no data</span>}
      </div>
      <div style={{ whiteSpace: "pre", lineHeight: "18px" }}>
        {status.node_name.map((name, i) => {
          const s = status.node_status[i] ?? 0;
          const on = s === 1 || s === 2;
          const kind = status.node_kind[i] ?? "";
          return (
            <div key={i}>
              <span style={{ color: p.faint }}>{lead[i]}</span>
              <span style={{ color: on ? activeColor : p.faint }}>{GLYPH[s]} </span>
              <span style={{ color: on ? activeColor : s === 3 ? p.muted : p.faint, fontWeight: on ? 600 : 400 }}>{name}</span>
              {kind !== name && <span style={{ color: p.faint }}> {kind}</span>}
            </div>
          );
        })}
      </div>
      <div style={{ borderTop: `1px solid ${p.line}`, marginTop: 10, paddingTop: 8 }}>
        <div style={{ display: "flex", flexWrap: "wrap", alignItems: "center", gap: 4 }}>
          {status.fsm_state.map((state, i) => (
            <span key={state} style={{ display: "inline-flex", alignItems: "center", gap: 4 }}>
              <span
                style={{
                  padding: "1px 6px",
                  borderRadius: 3,
                  border: `1px solid ${i === status.fsm_active ? activeColor : p.line}`,
                  color: i === status.fsm_active ? activeColor : p.muted,
                  fontWeight: i === status.fsm_active ? 600 : 400,
                }}
              >
                {state}
              </span>
              {i < status.fsm_state.length - 1 && <span style={{ color: p.faint }}>→</span>}
            </span>
          ))}
        </div>
        <div style={{ color: p.muted, marginTop: 4 }}>{status.fsm_note || " "}</div>
      </div>
    </div>
  );
}

export function initBehaviorBoard(context: PanelExtensionContext): () => void {
  const root = createRoot(context.panelElement);
  root.render(<BehaviorBoard context={context} />);
  return () => {
    root.unmount();
  };
}
