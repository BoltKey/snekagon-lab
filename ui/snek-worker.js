// The engine compiled to WebAssembly (snek.js + snek.wasm, built by web/build.sh), off the page's thread.
// Same protocol as `snek serve`: the page posts {id, cmd} and gets back {id, out} with the JSON answer.
importScripts('snek.js');
const engine = SnekEngine().then((m) => m.cwrap('snek_cmd', 'string', ['string']));
onmessage = async (e) => {
  const run = await engine;
  postMessage({ id: e.data.id, out: run(e.data.cmd) });
};
