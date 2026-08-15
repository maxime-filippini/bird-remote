import test from 'node:test';
import assert from 'node:assert/strict';
import { mkdtempSync, readFileSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';
import { tmpdir } from 'node:os';
import { createBirdServer, loadCatalog } from './server.mjs';

function assets() {
  const directory=mkdtempSync(join(tmpdir(),'birds-'));
  const images=[
    {file:'a.jpg',title:'Bird A',artist:'Alice',license:'CC BY 4.0',source_url:'https://example.com/a'},
    {file:'b.jpg',title:'Bird B',artist:'Bob',license:'CC0',source_url:'https://example.com/b'},
    {file:'deleted.jpg',title:'Gone',artist:'C',license:'CC0',source_url:'https://example.com/gone'}
  ];
  writeFileSync(join(directory,'metadata.json'),JSON.stringify({images}));
  writeFileSync(join(directory,'a.jpg'),'jpeg-a');writeFileSync(join(directory,'b.jpg'),'jpeg-b');
  return directory;
}

async function running(options={}) {
  const app=createBirdServer({assetDirectory:assets(),token:'secret',random:()=>0,...options});
  await new Promise(resolve=>app.server.listen(0,'127.0.0.1',resolve));
  const base=`http://127.0.0.1:${app.server.address().port}`;
  return {app,base,close:()=>new Promise(resolve=>app.server.close(resolve))};
}

test('display hides resolved status and keeps the current image visible while swapping',()=>{
  const html=readFileSync(new URL('./display.html',import.meta.url),'utf8');
  assert.match(html,/#status\[hidden\]\s*\{[^}]*display\s*:\s*none/);
  assert.doesNotMatch(html,/image\.classList\.remove\(['"]ready['"]\)/);
});

test('catalog follows JPEG directory and rejects unattributed JPEGs',()=>{
  const directory=assets();assert.equal(loadCatalog(directory).length,2);
  writeFileSync(join(directory,'orphan.jpg'),'x');
  assert.throws(()=>loadCatalog(directory),/lacks metadata/);
});

test('health, authentication, next selection, and curated image serving',async()=>{
  const instance=await running();
  try {
    let response=await fetch(`${instance.base}/health`);assert.equal(response.status,200);assert.deepEqual(await response.json(),{status:'ok'});
    const before=await (await fetch(`${instance.base}/api/current`)).json();
    assert.equal((await fetch(`${instance.base}/api/next`,{method:'POST'})).status,401);
    response=await fetch(`${instance.base}/api/next`,{method:'POST',headers:{Authorization:'Bearer secret'}});
    assert.equal(response.status,200);const after=await response.json();assert.notEqual(after.file,before.file);assert.equal(after.version,before.version+1);
    assert.equal((await fetch(`${instance.base}/birds/deleted.jpg`)).status,404);
    assert.equal(await (await fetch(`${instance.base}${after.image_url}`)).text(),`jpeg-${after.file[0]}`);
  } finally { await instance.close(); }
});
