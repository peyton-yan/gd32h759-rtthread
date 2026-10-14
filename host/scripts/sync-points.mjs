import { readFileSync, writeFileSync, mkdirSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, resolve } from "node:path";

// 把设备侧生成的点表拷进前端（单一事实来源）
const here = dirname(fileURLToPath(import.meta.url));
const src = resolve(here, "../../points.json");
const dst = resolve(here, "../src/lib/points.json");

mkdirSync(dirname(dst), { recursive: true });
writeFileSync(dst, readFileSync(src));
console.log(`synced ${src} -> ${dst}`);
