// Installed before every UI module, including direct fetch call sites.
(() => {
  const fetch = window.fetch.bind(window);
  window.fetch = (input, init) => {
    const url = new URL(input instanceof Request ? input.url : input, location.href);
    const port = String(window.RIG_PORT || location.port || 17770);
    if (url.protocol === "http:" && ["127.0.0.1", "localhost"].includes(url.hostname) &&
        url.port === port && url.pathname.startsWith("/api/")) {
      const headers = new Headers((init && init.headers) || (input instanceof Request ? input.headers : undefined));
      if (window.HEADROOM_API_TOKEN) headers.set("X-Headroom-Token", window.HEADROOM_API_TOKEN);
      init = Object.assign({}, init, { headers });
    }
    return fetch(input, init);
  };
})();
