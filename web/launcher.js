import {WebPlatform} from './platform.js';

const panel = document.querySelector('#log');
const status = document.querySelector('#status');
const start = document.querySelector('#start');
const canvas = document.querySelector('canvas');
const lines = [];
function log(level, message) {
  lines.push(`[${level}] ${message}`);
  if (lines.length > 500) lines.shift();
  panel.textContent = lines.join('\n');
  panel.scrollTop = panel.scrollHeight;
}
const platform = new WebPlatform(canvas, log, message => { status.textContent = message; });

start.addEventListener('click', async () => {
  start.disabled = true;
  status.textContent = 'Loading WASM…';
  try {
    const {default: createRuntime} = await import('./wiicompiled-web.js');
    await createRuntime({
      platform,
      print: message => log('INFO', message.replace(/^\[INFO\] /, '')),
      printErr: message => log('ERROR', message.replace(/^\[ERROR\] /, '')),
      onAbort: reason => platform.fail(`WASM aborted: ${reason}`),
    });
  } catch (error) {
    platform.fail(error);
  }
});

document.querySelector('#fullscreen').addEventListener('click', async () => {
  try {
    if (document.fullscreenElement) await document.exitFullscreen();
    else await canvas.requestFullscreen();
  } catch (error) { log('WARN', `Fullscreen failed: ${error.message}`); }
});
document.querySelector('#copy').addEventListener('click', async () => {
  try { await navigator.clipboard.writeText(lines.join('\n')); }
  catch (error) { log('WARN', `Copy failed; use Download Log: ${error.message}`); }
});
document.querySelector('#download').addEventListener('click', () => {
  const url = URL.createObjectURL(new Blob([lines.join('\n')], {type: 'text/plain'}));
  const link = document.createElement('a');
  link.href = url;
  link.download = 'wiicompiled-web.log';
  link.click();
  setTimeout(() => URL.revokeObjectURL(url), 1000);
});
log('INFO', `Cross-origin isolation: ${globalThis.crossOriginIsolated}; this probe uses no threads`);
log('DEBUG', 'No game files, translated code or Aurora are loaded by this experiment');
