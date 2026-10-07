/** Browser client for the versioned local API. No UI framework dependency. */
export class FathomAPIError extends Error {
  constructor(status, payload) {
    super(payload.error?.message ?? `HTTP ${status}`);
    this.status = status;
    this.code = payload.error?.code;
  }
}

export class FathomClient {
  constructor(baseURL = 'http://127.0.0.1:8765') {
    this.baseURL = baseURL.replace(/\/$/, '');
  }
  async request(method, path, body, signal) {
    const response = await fetch(this.baseURL + path, {
      method, signal,
      headers: body === undefined ? {} : { 'Content-Type': 'application/json' },
      body: body === undefined ? undefined : JSON.stringify(body),
    });
    const payload = await response.json();
    if (!response.ok) throw new FathomAPIError(response.status, payload);
    return payload;
  }
  models() { return this.request('GET', '/v1/models'); }
  describe(id) { return this.request('GET', `/v1/models/${encodeURIComponent(id)}/describe`); }
  run(request) { return this.request('POST', '/v1/runs', request); }
  status(id) { return this.request('GET', `/v1/runs/${encodeURIComponent(id)}`); }
  cancel(id) { return this.request('POST', `/v1/runs/${encodeURIComponent(id)}/cancel`, {}); }
  summary(id, request) { return this.request('POST', `/v1/runs/${encodeURIComponent(id)}/summary`, request); }
  compare(request) { return this.request('POST', '/v1/compare', request); }
  explain(id, request) { return this.request('POST', `/v1/runs/${encodeURIComponent(id)}/explain`, request); }
  results(id, filters = {}) {
    const query = new URLSearchParams();
    for (const [name, value] of Object.entries(filters)) {
      if (value === undefined) continue;
      for (const item of Array.isArray(value) ? value : [value]) query.append(name, String(item));
    }
    return this.request('GET', `/v1/runs/${encodeURIComponent(id)}/results?${query}`);
  }
  watch(id, onStatus, onError = () => {}) {
    const events = new EventSource(`${this.baseURL}/v1/runs/${encodeURIComponent(id)}/events`);
    events.addEventListener('status', event => {
      const status = JSON.parse(event.data);
      if (['completed', 'failed', 'cancelled', 'interrupted'].includes(status.status)) events.close();
      onStatus(status);
    });
    events.onerror = onError;
    return () => events.close(); // Stops listening; call cancel(id) to stop computation.
  }
}
