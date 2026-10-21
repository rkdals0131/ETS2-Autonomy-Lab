import { ExtensionContext } from "@foxglove/extension";

import { initBehaviorBoard } from "./BehaviorBoard";

export function activate(extensionContext: ExtensionContext): void {
  extensionContext.registerPanel({ name: "Behavior board", initPanel: initBehaviorBoard });
}
