const fs = require('node:fs');
const assert = require('node:assert/strict');
const path = require('node:path');

// 用模拟的浏览器 DOM 与轻量 C++ UIContext 状态机运行 WebInput.cpp 里真实的
// MiniGUIWeb_Install / MiniGUIWeb_Uninstall，验证防粘连拖动（sticky pan/drag）链路。
const sourceRaw = fs.readFileSync(path.join(__dirname, '../../src/UI/Platform/Web/WebInput.cpp'), 'utf8');
const source = sourceRaw.replace(/\r\n/g, '\n');

function emJsBody(name) {
  const head = source.indexOf('EM_JS(void, ' + name);
  assert.ok(head >= 0, 'EM_JS ' + name + ' declaration exists');
  const start = source.indexOf('{', head) + 1;
  const end = source.indexOf('\n    });', start);
  assert.ok(end > start, 'EM_JS ' + name + ' body exists');
  return asCompiled(source.slice(start, end));
}

// 与 RemoteOpenTests.cjs 保持一致：按 EM_JS 的 C 字符串转义规则还原浏览器里真正执行的 JS。
function asCompiled(js) {
  let out = '';
  for (let i = 0; i < js.length;) {
    const c = js[i];
    if (c === '"' || c === "'") {
      let j = i + 1;
      while (j < js.length && js[j] !== c) j += js[j] === '\\' ? 2 : 1;
      out += js.slice(i, j + 1);
      i = j + 1;
    } else if (c === '\\') {
      const n = js[i + 1];
      if (n === 'x') { out += String.fromCharCode(parseInt(js.substr(i + 2, 2), 16)); i += 4; continue; }
      out += ({ n: '\n', t: '\t', '\\': '\\' })[n] ?? n;
      i += 2;
    } else {
      out += c;
      ++i;
    }
  }
  return out;
}

const installBody = emJsBody('MiniGUIWeb_Install');
const uninstallBody = emJsBody('MiniGUIWeb_Uninstall');

// 检查 C++ 源码的 WebInput::OnFocusLost 与 WebInput::OnMouse 静态契约，用于让模拟 C++ 侧真实反映源码行为。
function extractCppFunctionBody(src, signature) {
  const idx = src.indexOf(signature);
  if (idx < 0) return '';
  const open = src.indexOf('{', idx);
  if (open < 0) return '';
  let depth = 0;
  for (let i = open; i < src.length; ++i) {
    if (src[i] === '{') ++depth;
    else if (src[i] === '}') {
      --depth;
      if (depth === 0) return src.slice(open, i + 1);
    }
  }
  return '';
}

const onFocusLostCpp = extractCppFunctionBody(source, 'void WebInput::OnFocusLost()');
const onMouseCpp = extractCppFunctionBody(source, 'bool WebInput::OnMouse(');
const cppOnFocusLostCallsPointerCancel = /m_context->PointerCancel\(\)/.test(onFocusLostCpp);
const cppOnMouseHandlesCancel = /case\s+MouseCancel\s*:\s*m_context->PointerCancel\(\)\s*;/.test(onMouseCpp);

function createEventTarget(name) {
  const listeners = [];
  return {
    name,
    listeners,
    addEventListener(type, fn, opts) {
      listeners.push({ type, fn, opts });
    },
    removeEventListener(type, fn, opts) {
      const idx = listeners.findIndex(item => item.type === type && item.fn === fn && item.opts === opts);
      if (idx >= 0) listeners.splice(idx, 1);
    },
    dispatch(type, event = {}) {
      const snapshot = listeners.filter(item => item.type === type);
      for (const item of snapshot) item.fn(event);
    },
    listenerCount(type) {
      return type ? listeners.filter(item => item.type === type).length : listeners.length;
    },
  };
}

function makePointerEvent(overrides = {}) {
  let defaultPrevented = false;
  return {
    clientX: 110,
    clientY: 120,
    button: 0,
    buttons: 0,
    shiftKey: false,
    ctrlKey: false,
    metaKey: false,
    altKey: false,
    timeStamp: 100,
    pointerId: 1,
    pointerType: 'mouse',
    preventDefault() { defaultPrevented = true; },
    get defaultPrevented() { return defaultPrevented; },
    ...overrides,
  };
}

function createHarness({ setCaptureThrows = false, rect = { left: 10, top: 20, width: 400, height: 300 } } = {}) {
  const capturedIds = new Set();
  const captureLog = [];
  const canvasTarget = createEventTarget('canvas');
  const canvas = Object.assign(canvasTarget, {
    style: { cursor: '' },
    getBoundingClientRect: () => rect,
    setPointerCapture(id) {
      captureLog.push(['set', id]);
      if (setCaptureThrows) throw new Error('InvalidStateError: cannot capture');
      capturedIds.add(id);
    },
    hasPointerCapture(id) {
      return capturedIds.has(id);
    },
    releasePointerCapture(id) {
      captureLog.push(['release', id]);
      capturedIds.delete(id);
    },
  });

  const taTarget = createEventTarget('textarea');
  let taRemoved = false;
  const ta = Object.assign(taTarget, {
    value: '',
    style: { cssText: '' },
    attributes: new Map(),
    setAttribute(k, v) { this.attributes.set(k, v); },
    focus() { document.activeElement = ta; },
    blur() { if (document.activeElement === ta) document.activeElement = null; },
    remove() { taRemoved = true; },
  });

  const windowObj = createEventTarget('window');
  const documentTarget = createEventTarget('document');
  const document = Object.assign(documentTarget, {
    hidden: false,
    activeElement: null,
    body: {
      children: [],
      appendChild(el) { this.children.push(el); },
    },
    querySelector(sel) { return sel === '#canvas' ? canvas : null; },
    createElement(tag) {
      assert.equal(tag, 'textarea');
      return ta;
    },
  });

  const Module = {};
  const mouseCalls = [];
  const ctxLog = [];
  const ctx = {
    buttons: 0,
    capture: null,
    pointerCancelCount: 0,
    keyboardFocusLostCount: 0,
  };
  let focusLostCalls = 0;

  const buttonBit = (b) => (b === 0 || b === 1 || b === 2 ? (1 << b) : 0);

  const onMouse = (type, x, y, button, mods, ts) => {
    mouseCalls.push({ type, x, y, button, mods, ts });
    if (type === 1) {
      ctx.buttons |= buttonBit(button);
      ctx.capture = 'viewport';
      ctxLog.push({ op: 'PointerDown', button, x, y, buttons: ctx.buttons });
    } else if (type === 2) {
      ctx.buttons &= ~buttonBit(button);
      ctxLog.push({ op: 'PointerUp', button, x, y, buttons: ctx.buttons });
      if (ctx.buttons === 0) ctx.capture = null;
    } else if (type === 0) {
      ctxLog.push({ op: 'PointerMove', x, y, buttons: ctx.buttons, captured: ctx.capture !== null });
    } else if (type === 3) {
      ctxLog.push({ op: 'PointerLeave', buttons: ctx.buttons });
    } else if (type === 4) {
      if (cppOnMouseHandlesCancel) {
        const hadCapture = ctx.capture !== null;
        ctx.buttons = 0;
        ctx.capture = null;
        ++ctx.pointerCancelCount;
        ctxLog.push({ op: 'PointerCancel', hadCapture, buttons: ctx.buttons });
      }
    }
    return 1;
  };

  const onFocusLost = () => {
    ++focusLostCalls;
    if (cppOnFocusLostCallsPointerCancel) {
      const hadCapture = ctx.capture !== null;
      ctx.buttons = 0;
      ctx.capture = null;
      ++ctx.pointerCancelCount;
      ctxLog.push({ op: 'PointerCancel', hadCapture, buttons: ctx.buttons });
    }
    ++ctx.keyboardFocusLostCount;
    ctxLog.push({ op: 'KeyboardFocusLost', buttons: ctx.buttons });
  };

  const install = new Function(
    'selectorPtr', 'document', 'window', 'Module', 'console',
    'UTF8ToString', 'stringToNewUTF8', '_free',
    '_MiniGUIWeb_OnMouse', '_MiniGUIWeb_OnWheel', '_MiniGUIWeb_OnKey',
    '_MiniGUIWeb_OnText', '_MiniGUIWeb_OnPaste', '_MiniGUIWeb_OnComposition',
    '_MiniGUIWeb_OnFocusLost',
    installBody,
  );
  const uninstall = new Function('Module', uninstallBody);

  install(
    '#canvas', document, windowObj, Module, console,
    s => s, s => s, () => {},
    onMouse, () => 0, () => 0,
    () => {}, () => {}, () => {},
    onFocusLost,
  );

  return {
    canvas,
    ta,
    window: windowObj,
    document,
    Module,
    capturedIds,
    captureLog,
    mouseCalls,
    ctxLog,
    ctx,
    get focusLostCalls() { return focusLostCalls; },
    get taRemoved() { return taRemoved; },
    uninstall: () => uninstall(Module),
  };
}

const failures = [];
function runCase(name, fn) {
  try {
    fn();
  } catch (err) {
    failures.push({ name, err });
  }
}

// 1. 正常中键按下 -> 移动 -> 抬起（无误取消、无多余事件）
runCase('1. normal middle-button pan down -> move -> up', () => {
  const h = createHarness();
  const pd = makePointerEvent({ button: 1, buttons: 4, pointerId: 7, clientX: 60, clientY: 80, timeStamp: 10 });
  const md = makePointerEvent({ button: 1, buttons: 4, clientX: 60, clientY: 80, timeStamp: 11 });
  const mm = makePointerEvent({ button: 0, buttons: 4, clientX: 90, clientY: 100, timeStamp: 12 });
  const mu = makePointerEvent({ button: 1, buttons: 0, clientX: 90, clientY: 100, timeStamp: 13 });

  h.canvas.dispatch('pointerdown', pd);
  h.canvas.dispatch('mousedown', md);
  assert.equal(md.defaultPrevented, true, 'mousedown should preventDefault');
  assert.equal(h.capturedIds.has(7), true, 'pointerId 7 should be captured while dragging');

  h.window.dispatch('mousemove', mm);
  h.window.dispatch('mouseup', mu);

  assert.deepEqual(
    h.mouseCalls.map(c => [c.type, c.x, c.y, c.button]),
    [
      [1, 50, 60, 1],
      [0, 80, 80, -1],
      [2, 80, 80, 1],
    ],
    'exact Down -> Move -> Up sequence with no stray cancel or duplicate events',
  );
  assert.equal(h.capturedIds.size, 0, 'pointer capture released on mouseup');
  assert.equal(h.ctx.buttons, 0, 'UIContext m_buttons cleared');
  assert.equal(h.ctx.capture, null, 'UIContext m_capture released');
  assert.equal(h.ctx.pointerCancelCount, 0, 'no spurious PointerCancel during normal drag');
});

// 2. 中键拖出跨源 iframe 丢 mouseup 后移回（mousemove 带 e.buttons === 0）
runCase('2. dropped mouseup recovers on next mousemove via e.buttons reconciliation', () => {
  const h = createHarness();
  h.canvas.dispatch('pointerdown', makePointerEvent({ button: 1, buttons: 4, pointerId: 9, clientX: 50, clientY: 50 }));
  h.canvas.dispatch('mousedown', makePointerEvent({ button: 1, buttons: 4, clientX: 50, clientY: 50, timeStamp: 20 }));
  // 拖出期间 mouseup 丢失；用户松开中键后移回画布，收到 mousemove(buttons === 0)
  h.window.dispatch('mousemove', makePointerEvent({ buttons: 0, clientX: 120, clientY: 140, timeStamp: 30 }));
  // 后续继续移动：不再带中键状态
  h.window.dispatch('mousemove', makePointerEvent({ buttons: 0, clientX: 135, clientY: 155, timeStamp: 40 }));

  assert.deepEqual(
    h.mouseCalls.map(c => [c.type, c.x, c.y, c.button]),
    [
      [1, 40, 30, 1],   // 原始中键 Down
      [2, 110, 120, 1], // 先补发中键 Up
      [0, 110, 120, -1], // 再发 Move
      [0, 125, 135, -1], // 后续 Move
    ],
    'must synthesize MouseUp(1) before MouseMove when e.buttons === 0',
  );
  assert.deepEqual(
    h.ctxLog.filter(e => e.op === 'PointerMove').map(e => ({ buttons: e.buttons, captured: e.captured })),
    [
      { buttons: 0, captured: false },
      { buttons: 0, captured: false },
    ],
    'neither MouseMove after recovery may carry middle-button drag state',
  );
  assert.equal(h.capturedIds.size, 0, 'pointer capture released once all buttons reconciled to 0');

  // 补充：若丢失 mouseup 后首次 mousemove 仍在画布外部（buttons === 0），也应先补发 MouseUp 并阻止画布外多余 Move
  const h2 = createHarness();
  h2.canvas.dispatch('mousedown', makePointerEvent({ button: 1, buttons: 4, clientX: 50, clientY: 50 }));
  h2.window.dispatch('mousemove', makePointerEvent({ buttons: 0, clientX: 600, clientY: 500 }));
  assert.deepEqual(
    h2.mouseCalls.map(c => [c.type, c.button]),
    [[1, 1], [2, 1]],
    'outside-canvas mousemove with buttons===0 should synthesize MouseUp and skip out-of-bounds MouseMove',
  );
});

// 3. W3C e.buttons 中右键位映射精确验证（Left=1, Right=2, Middle=4）
runCase('3. W3C e.buttons bitmask mapping distinguishes middle (4) and right (2)', () => {
  // 3a: 同时按下左(0)+中(1)+右(2)后，mousemove 带 e.buttons === 4（仅中键按着）
  const hMid = createHarness();
  hMid.canvas.dispatch('mousedown', makePointerEvent({ button: 0, buttons: 1, clientX: 30, clientY: 40 }));
  hMid.canvas.dispatch('mousedown', makePointerEvent({ button: 1, buttons: 5, clientX: 30, clientY: 40 }));
  hMid.canvas.dispatch('mousedown', makePointerEvent({ button: 2, buttons: 7, clientX: 30, clientY: 40 }));
  hMid.mouseCalls.length = 0;

  hMid.window.dispatch('mousemove', makePointerEvent({ buttons: 4, clientX: 70, clientY: 80 }));
  assert.deepEqual(
    hMid.mouseCalls.map(c => [c.type, c.button]),
    [
      [2, 0],  // 补发左键 Up
      [2, 2],  // 补发右键 Up
      [0, -1], // 随后 Move
    ],
    'e.buttons===4 must release left(0) & right(2) and keep middle(1)',
  );
  assert.equal(hMid.ctx.buttons, 1 << 1, 'only middle button should remain pressed in UIContext');

  // 3b: 同时按下左(0)+中(1)+右(2)后，mousemove 带 e.buttons === 2（仅右键按着）
  const hRight = createHarness();
  hRight.canvas.dispatch('mousedown', makePointerEvent({ button: 0, buttons: 1, clientX: 30, clientY: 40 }));
  hRight.canvas.dispatch('mousedown', makePointerEvent({ button: 1, buttons: 5, clientX: 30, clientY: 40 }));
  hRight.canvas.dispatch('mousedown', makePointerEvent({ button: 2, buttons: 7, clientX: 30, clientY: 40 }));
  hRight.mouseCalls.length = 0;

  hRight.window.dispatch('mousemove', makePointerEvent({ buttons: 2, clientX: 70, clientY: 80 }));
  assert.deepEqual(
    hRight.mouseCalls.map(c => [c.type, c.button]),
    [
      [2, 0],  // 补发左键 Up
      [2, 1],  // 补发中键 Up
      [0, -1], // 随后 Move
    ],
    'e.buttons===2 must release left(0) & middle(1) and keep right(2)',
  );
  assert.equal(hRight.ctx.buttons, 1 << 2, 'only right button should remain pressed in UIContext');
});

// 4. window.blur：释放 pointerCapture、清 JS buttons、调用 _MiniGUIWeb_OnFocusLost()，且 C++ OnFocusLost() 调用 PointerCancel()
runCase('4. window.blur releases capture, clears buttons, and C++ OnFocusLost invokes PointerCancel', () => {
  // C++ 源码静态契约检查
  assert.match(
    source,
    /enum\s+MouseType\s*\{[^}]*MouseCancel\s*=\s*4[^}]*\}/,
    'WebInput.cpp MouseType enum must define MouseCancel = 4',
  );
  assert.equal(cppOnMouseHandlesCancel, true, 'WebInput::OnMouse must handle MouseCancel via m_context->PointerCancel()');
  assert.equal(cppOnFocusLostCallsPointerCancel, true, 'WebInput::OnFocusLost must call m_context->PointerCancel()');
  const pcIdx = onFocusLostCpp.indexOf('m_context->PointerCancel()');
  const kfIdx = onFocusLostCpp.indexOf('m_context->KeyboardFocusLost()');
  const acIdx = onFocusLostCpp.indexOf('ApplyCursor()');
  assert.ok(pcIdx >= 0 && kfIdx > pcIdx && acIdx > kfIdx, 'OnFocusLost must call PointerCancel -> KeyboardFocusLost -> ApplyCursor in order');

  const h = createHarness();
  h.canvas.dispatch('pointerdown', makePointerEvent({ button: 1, buttons: 4, pointerId: 11 }));
  h.canvas.dispatch('mousedown', makePointerEvent({ button: 1, buttons: 4 }));
  assert.equal(h.capturedIds.has(11), true);

  h.window.dispatch('blur');
  assert.equal(h.focusLostCalls, 1, 'blur must call _MiniGUIWeb_OnFocusLost');
  assert.equal(h.capturedIds.size, 0, 'blur must release pointer capture');
  assert.equal(h.ctx.buttons, 0, 'UIContext m_buttons must be cleared by OnFocusLost -> PointerCancel');
  assert.equal(h.ctx.capture, null, 'UIContext m_capture must be cleared by OnFocusLost -> PointerCancel');

  // blur 后 JS buttons 已清零：迟到的 mouseup 不应重复发 Up，画布外 mousemove 应被裁剪
  h.mouseCalls.length = 0;
  h.window.dispatch('mouseup', makePointerEvent({ button: 1, buttons: 0 }));
  h.window.dispatch('mousemove', makePointerEvent({ buttons: 0, clientX: 999, clientY: 999 }));
  assert.equal(h.mouseCalls.length, 0, 'JS buttons must be cleared on blur');
});

// 5. document.visibilitychange：document.hidden === true 时清理，false 时不误触发
runCase('5. document.visibilitychange handles hidden=true and ignores hidden=false', () => {
  const h = createHarness();
  h.canvas.dispatch('pointerdown', makePointerEvent({ button: 1, buttons: 4, pointerId: 12 }));
  h.canvas.dispatch('mousedown', makePointerEvent({ button: 1, buttons: 4 }));

  // document.hidden === false 不误触发
  h.document.hidden = false;
  h.document.dispatch('visibilitychange');
  assert.equal(h.focusLostCalls, 0, 'visibilitychange with hidden=false must not trigger focus lost');
  assert.equal(h.capturedIds.has(12), true, 'capture must stay intact while document is visible');
  assert.equal(h.ctx.buttons, 1 << 1, 'middle button state must remain intact while visible');

  // document.hidden === true 触发清理
  h.document.hidden = true;
  h.document.dispatch('visibilitychange');
  assert.equal(h.focusLostCalls, 1, 'visibilitychange with hidden=true must trigger _MiniGUIWeb_OnFocusLost');
  assert.equal(h.capturedIds.size, 0, 'visibilitychange hidden=true must release pointer capture');
  assert.equal(h.ctx.buttons, 0, 'UIContext m_buttons cleared on hidden=true');

  // JS buttons 已清零
  h.mouseCalls.length = 0;
  h.window.dispatch('mouseup', makePointerEvent({ button: 1, buttons: 0 }));
  assert.equal(h.mouseCalls.length, 0, 'JS buttons must be cleared when hidden=true');
});

// 6. pointercancel 与 Module.MiniGUIWebInput.cancelPointer()
runCase('6. pointercancel and Module.MiniGUIWebInput.cancelPointer emit MouseCancel(4)', () => {
  const h = createHarness();
  // 未按任何键时 pointercancel 不误发 MouseCancel
  h.window.dispatch('pointercancel', makePointerEvent({ timeStamp: 50 }));
  assert.equal(h.mouseCalls.length, 0, 'pointercancel with no pressed buttons should not emit MouseCancel');

  // 中键按下后触发 pointercancel -> 发 MouseCancel (type=4)，清 buttons，释放 capture
  h.canvas.dispatch('pointerdown', makePointerEvent({ button: 1, buttons: 4, pointerId: 13 }));
  h.canvas.dispatch('mousedown', makePointerEvent({ button: 1, buttons: 4 }));
  h.mouseCalls.length = 0;
  h.window.dispatch('pointercancel', makePointerEvent({ shiftKey: true, timeStamp: 77 }));
  assert.deepEqual(
    h.mouseCalls,
    [{ type: 4, x: 0, y: 0, button: -1, mods: 1, ts: 77 }],
    'pointercancel during active drag must emit MouseCancel(4)',
  );
  assert.equal(h.capturedIds.size, 0, 'pointercancel must release capture');
  assert.equal(h.ctx.buttons, 0, 'UIContext m_buttons cleared on MouseCancel');

  // 确认 JS buttons 已被清空
  h.mouseCalls.length = 0;
  h.window.dispatch('mouseup', makePointerEvent({ button: 1, buttons: 0 }));
  assert.equal(h.mouseCalls.length, 0, 'JS buttons cleared after pointercancel');

  // 宿主挂起调用 Module.MiniGUIWebInput.cancelPointer()：有按键时清空并发 MouseCancel(4)
  assert.equal(typeof h.Module.MiniGUIWebInput.cancelPointer, 'function', 'Module.MiniGUIWebInput.cancelPointer must be exposed');
  h.canvas.dispatch('pointerdown', makePointerEvent({ button: 1, buttons: 4, pointerId: 14 }));
  h.canvas.dispatch('mousedown', makePointerEvent({ button: 1, buttons: 4 }));
  h.mouseCalls.length = 0;
  h.Module.MiniGUIWebInput.cancelPointer();
  assert.deepEqual(
    h.mouseCalls,
    [{ type: 4, x: 0, y: 0, button: -1, mods: 0, ts: 0 }],
    'host cancelPointer() must emit MouseCancel(4)',
  );
  assert.equal(h.capturedIds.size, 0, 'host cancelPointer() must release capture');
  assert.equal(h.ctx.buttons, 0, 'host cancelPointer() clears UIContext m_buttons');

  // 即使 JS 侧无按键，宿主调用 cancelPointer() 也应无条件通知 C++ PointerCancel 实现安全双清
  h.mouseCalls.length = 0;
  h.Module.MiniGUIWebInput.cancelPointer();
  assert.deepEqual(
    h.mouseCalls,
    [{ type: 4, x: 0, y: 0, button: -1, mods: 0, ts: 0 }],
    'host cancelPointer() must unconditionally notify C++ PointerCancel',
  );
});

// 7. 鼠标侧键（button 3 / 4）不污染 buttons，不调用 _MiniGUIWeb_OnMouse
runCase('7. mouse side buttons (3 and 4) do not pollute buttons or call _MiniGUIWeb_OnMouse', () => {
  const h = createHarness();
  for (const sideBtn of [3, 4]) {
    const pd = makePointerEvent({ button: sideBtn, buttons: sideBtn === 3 ? 8 : 16, pointerId: 20 + sideBtn });
    const md = makePointerEvent({ button: sideBtn, buttons: sideBtn === 3 ? 8 : 16 });
    const mu = makePointerEvent({ button: sideBtn, buttons: 0 });
    h.canvas.dispatch('pointerdown', pd);
    h.canvas.dispatch('mousedown', md);
    h.window.dispatch('mouseup', mu);
    assert.equal(md.defaultPrevented, false, 'side button mousedown must be ignored without preventDefault');
  }
  assert.equal(h.capturedIds.size, 0, 'side button pointerdown must not acquire capture');
  assert.equal(h.mouseCalls.length, 0, 'side buttons must never invoke _MiniGUIWeb_OnMouse');

  // 再次验证 buttons 未被污染：mouseleave 仍应照常发出 MouseLeave(3)
  h.canvas.dispatch('mouseleave', makePointerEvent({ timeStamp: 99 }));
  assert.deepEqual(
    h.mouseCalls.map(c => c.type),
    [3],
    'buttons must remain 0 after side buttons so mouseleave works normally',
  );
});

// 8. pointerdown 指针捕获：正常捕获、不 preventDefault、抛异常静默容错、touch 不捕获
runCase('8. pointerdown capture behavior, error resilience, and touch exclusion', () => {
  // 正常鼠标 pointerdown 捕获且不调用 preventDefault
  const h = createHarness();
  const mousePd = makePointerEvent({ button: 0, pointerId: 31, pointerType: 'mouse' });
  h.canvas.dispatch('pointerdown', mousePd);
  assert.equal(h.capturedIds.has(31), true, 'mouse pointerdown must call setPointerCapture');
  assert.equal(mousePd.defaultPrevented, false, 'pointerdown must not call preventDefault');

  // touch pointerdown 不捕获
  const touchPd = makePointerEvent({ button: 0, pointerId: 32, pointerType: 'touch' });
  h.canvas.dispatch('pointerdown', touchPd);
  assert.equal(h.capturedIds.has(32), false, 'touch pointerdown must not call setPointerCapture');

  // setPointerCapture 抛异常时静默吞掉且不阻断交互
  const hThrow = createHarness({ setCaptureThrows: true });
  assert.doesNotThrow(() => {
    hThrow.canvas.dispatch('pointerdown', makePointerEvent({ button: 1, pointerId: 33, pointerType: 'mouse' }));
    hThrow.canvas.dispatch('mousedown', makePointerEvent({ button: 1, buttons: 4, clientX: 60, clientY: 70 }));
    hThrow.window.dispatch('mousemove', makePointerEvent({ buttons: 4, clientX: 80, clientY: 90 }));
    hThrow.window.dispatch('mouseup', makePointerEvent({ button: 1, buttons: 0, clientX: 80, clientY: 90 }));
  });
  assert.deepEqual(
    hThrow.mouseCalls.map(c => c.type),
    [1, 0, 2],
    'failed setPointerCapture must not block normal mouse interaction',
  );
});

// 9. MiniGUIWeb_Uninstall 卸载后所有监听全部移除
runCase('9. MiniGUIWeb_Uninstall removes every registered listener cleanly', () => {
  const h = createHarness();
  assert.ok(h.canvas.listenerCount('pointerdown') > 0, 'pointerdown registered on canvas');
  assert.ok(h.window.listenerCount('pointercancel') > 0, 'pointercancel registered on window');
  assert.ok(h.document.listenerCount('visibilitychange') > 0, 'visibilitychange registered on document');

  h.uninstall();

  assert.equal(h.canvas.listenerCount(), 0, 'all canvas listeners removed');
  assert.equal(h.window.listenerCount(), 0, 'all window listeners removed');
  assert.equal(h.document.listenerCount(), 0, 'all document listeners removed');
  assert.equal(h.ta.listenerCount(), 0, 'all textarea listeners removed');
  assert.equal(h.taRemoved, true, 'textarea removed on uninstall');
  assert.equal('MiniGUIWebInput' in h.Module, false, 'Module.MiniGUIWebInput deleted');

  // 卸载后再派发事件零回调
  h.canvas.dispatch('mousedown', makePointerEvent({ button: 1, buttons: 4 }));
  h.window.dispatch('blur');
  h.window.dispatch('pointercancel', makePointerEvent());
  h.document.hidden = true;
  h.document.dispatch('visibilitychange');
  assert.equal(h.mouseCalls.length, 0);
  assert.equal(h.focusLostCalls, 0);
});

if (failures.length > 0) {
  for (const { name, err } of failures) {
    console.error(`FAIL: ${name}\n  ${err.message}`);
  }
  process.exitCode = 1;
} else {
  console.log('PASS: WebInput sticky-drag prevention (e.buttons reconcile, W3C mask, pointer capture, blur/visibility/pointercancel/cancelPointer, side buttons, uninstall).');
}
