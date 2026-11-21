// Portions Copyright 2026 The Hollycast Authors
import * as fs from 'node:fs/promises';
import { randomBytes } from 'node:crypto';

export async function publishJson(file, value, { replace = false, beforePublish } = {}) {
  const temporary = `${file}.${randomBytes(8).toString('hex')}.tmp`;
  const handle = await fs.open(temporary, 'wx');
  try {
    try { await handle.writeFile(`${JSON.stringify(value, null, 2)}\n`); }
    finally { await handle.close(); }
    beforePublish?.();
    if (replace) {
      await fs.rename(temporary, file);
    } else {
      // Hard-link publication refuses replacement even when two writers race.
      try { await fs.link(temporary, file); }
      catch (error) {
        if (error.code === 'EEXIST') error.message = `Refusing to overwrite evidence: ${file}`;
        throw error;
      }
    }
  } finally {
    await fs.rm(temporary, { force: true });
  }
}

function redactObservation(ui, textItemId) {
  if (!Array.isArray(ui?.items)) return;
  const activePasswordId = ui.text_input?.password === true ? ui.text_input.active_item_id : null;
  for (const item of ui.items) {
    const password = item.password === true || (Number.isInteger(activePasswordId) && item.id === activePasswordId);
    const typedText = item.inputable === true && ['text', 'path', 'file'].includes(item.kind);
    const target = Number.isInteger(textItemId) && item.id === textItemId;
    if (!target && !password && !typedText && item.text_input_focused !== true) continue;
    for (const key of ['value', 'value_label', 'draft_value']) {
      if (!Object.hasOwn(item, key)) continue;
      if (password || (typeof item[key] === 'string' && (key !== 'value_label' || typedText)))
        item[key] = '[redacted]';
    }
    if (password || typedText || target) item.text_value_redacted = true;
  }
}

export function redactUiEvidence(value, textItemId) {
  function redact(node) {
    if (!node || typeof node !== 'object') return;
    redactObservation(node.ui, textItemId);
    for (const child of Object.values(node)) redact(child);
  }
  const copy = structuredClone(value);
  redact(copy);
  return copy;
}
