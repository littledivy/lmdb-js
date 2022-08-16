import { bench, run } from "mitata";
import { open } from '../node-index.js';
import fs from 'fs';

var testDirPath = new URL('./benchdata', import.meta.url).toString().slice(8);
fs.rmdirSync(testDirPath, { recursive: true });
fs.mkdirSync(testDirPath, { recursive: true });
let c = 0;

let data = {
    name: 'test',
    greeting: 'Hello, World!',
    flag: true,
    littleNum: 3,
    biggerNum: 32254435,
    decimal:1.332232,
    bigDecimal: 3.5522E102,
    negative: -54,
    aNull: null,
    more: 'string',
};
const rootStore = open(testDirPath, {
    noMemInit: true,
    pageSize: 0x4000,
});
const store = rootStore.openDB('testing', {
  create: true,
  sharedStructuresKey: 100000000,
  keyIsUint32: true,
});

for (let i = 0; i < 100; i++) {
  await store.put(i, data)
}

async function setData() {
    let key = (c += 357) % 100
    await store.put(key, data)
}

async function getData() {
    await store.get((c += 357) % 100)
}

function getBinaryFast() {
  store.getBinaryFast((c += 357) % 100)
}

bench('getBinaryFast', getBinaryFast);
bench("setData", setData);
bench("getData", getData);

await run();