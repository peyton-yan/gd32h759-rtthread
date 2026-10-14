import { writable } from "svelte/store";

// 每个点保留的最近样本数（@500ms 约 10 分钟）
export const CAPACITY = 1200;

const data = new Map(); // name -> { t: number[], v: number[] }

// 每次写入后自增，驱动图表重绘
export const revision = writable(0);

export function push(name, value, t = Date.now()) {
  if (typeof value !== "number" || Number.isNaN(value)) return;
  let s = data.get(name);
  if (!s) {
    s = { t: [], v: [] };
    data.set(name, s);
  }
  s.t.push(t);
  s.v.push(value);
  if (s.v.length > CAPACITY) {
    s.t.shift();
    s.v.shift();
  }
}

export function getSeries(name) {
  const s = data.get(name);
  return s ? s.v : [];
}

export function bump() {
  revision.update((n) => n + 1);
}

export function clearAll() {
  data.clear();
  bump();
}

/** 长表格式导出：time,name,value */
export function toCsv() {
  const rows = ["time,name,value"];
  for (const [name, s] of data) {
    for (let i = 0; i < s.v.length; i++) {
      rows.push(`${s.t[i]},${name},${s.v[i]}`);
    }
  }
  return rows.join("\n");
}
