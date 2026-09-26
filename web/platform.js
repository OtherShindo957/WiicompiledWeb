export class WebPlatform {
  constructor(canvas, log, setStatus) {
    this.canvas = canvas;
    this.log = log;
    this.setStatus = setStatus;
    this.running = false;
  }

  fail(error) {
    this.running = false;
    this.setStatus('Failed — reload to retry');
    this.log('ERROR', error instanceof Error ? error.message : String(error));
  }

  async initialize() {
    try {
      if (!globalThis.isSecureContext) throw new Error('Use HTTPS or localhost for WebGPU.');
      if (!navigator.gpu) throw new Error('WebGPU is unavailable in this browser.');
      this.setStatus('Requesting WebGPU adapter…');
      const adapter = await navigator.gpu.requestAdapter();
      if (!adapter) throw new Error('No WebGPU adapter is available.');
      this.device = await adapter.requestDevice();
      this.device.lost.then(info => this.fail(`WebGPU device lost: ${info.reason}: ${info.message}`));
      this.device.addEventListener('uncapturederror', event => this.fail(event.error));
      this.context = this.canvas.getContext('webgpu');
      if (!this.context) throw new Error('Could not create the canvas WebGPU context.');
      this.context.configure({
        device: this.device,
        format: navigator.gpu.getPreferredCanvasFormat(),
        alphaMode: 'opaque',
      });
      this.running = true;
      this.device.pushErrorScope('validation');
      if (!this.frame()) return false;
      await this.device.queue.onSubmittedWorkDone();
      const error = await this.device.popErrorScope();
      if (error) throw new Error(`Initial GPU clear failed: ${error.message}`);
      if (!this.running) return false;
      this.log('INFO', 'WebGPU canvas clear submitted and completed');
      this.setStatus('Running — WebGPU clear-screen probe');
      return true;
    } catch (error) {
      this.fail(error);
      return false;
    }
  }

  frame() {
    if (!this.running) return false;
    try {
      const scale = globalThis.devicePixelRatio || 1;
      const limit = this.device.limits.maxTextureDimension2D;
      const width = Math.max(1, Math.min(limit, Math.round(this.canvas.clientWidth * scale)));
      const height = Math.max(1, Math.min(limit, Math.round(this.canvas.clientHeight * scale)));
      if (this.canvas.width !== width || this.canvas.height !== height) {
        this.canvas.width = width;
        this.canvas.height = height;
      }
      const encoder = this.device.createCommandEncoder({label: 'Probe clear'});
      const pass = encoder.beginRenderPass({colorAttachments: [{
        view: this.context.getCurrentTexture().createView(),
        clearValue: {r: 0.025, g: 0.16, b: 0.24, a: 1},
        loadOp: 'clear', storeOp: 'store',
      }]});
      pass.end();
      this.device.queue.submit([encoder.finish()]);
      return true;
    } catch (error) {
      this.fail(error);
      return false;
    }
  }
}
