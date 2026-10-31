// The disc's range endpoint when the page is hosted: /disc, proxied from the
// URL in DISC_URL -- a project environment variable, set in the host's
// dashboard and never in this repository, which is public and the disc is
// the game. With it set, the page's plain URL works; without it, the page
// can still be opened with ?disc=<url> and reads the disc itself.
//
// DiscIO's HttpBlobReader asks for ranges and nothing else, so this serves
// ranges and nothing else, passing the request's Range up and the answer's
// status, length and range back, streamed.
export default async function handler(req, res) {
  const url = process.env.DISC_URL;
  if (!url) {
    res.status(503).send('DISC_URL is not set in the project environment; open the page with ?disc=<url>');
    return;
  }
  if (req.method !== 'GET' && req.method !== 'HEAD') {
    res.status(405).end();
    return;
  }
  const range = req.headers.range;
  if (!/^bytes=\d+-\d*$/.test(range ?? '')) {
    res.status(416).send('this endpoint serves ranges only');
    return;
  }
  const upstream = await fetch(url, { method: req.method, headers: { Range: range } });
  res.status(upstream.status);
  for (const h of ['content-range', 'content-length', 'accept-ranges', 'content-type', 'etag']) {
    const v = upstream.headers.get(h);
    if (v) res.setHeader(h, v);
  }
  if (req.method === 'HEAD' || !upstream.body) {
    res.end();
    return;
  }
  const reader = upstream.body.getReader();
  for (;;) {
    const { done, value } = await reader.read();
    if (done) break;
    res.write(Buffer.from(value));
  }
  res.end();
}
