<script>
  import { onMount } from "svelte";
  import points from "./lib/points.json";
  import * as api from "./lib/api.js";
  import * as history from "./lib/history.js";
  import { revision } from "./lib/history.js";
  import Trend from "./lib/Trend.svelte";

  const PALETTE = [
    "#38bdf8", "#f472b6", "#22c55e", "#f59e0b",
    "#a78bfa", "#f87171", "#2dd4bf", "#eab308",
  ];

  let host = "192.168.1.30";
  let port = 502;
  let pollMs = 500;
  let connected = false;
  let status = "未连接";
  let values = {};
  let errors = {};
  let diag = "";
  let normalize = true;   /* 趋势：各序列按自身量程独立缩放 */

  // 扁平化点表；为可写点准备草稿值
  const groups = Object.entries(points.areas).map(([area, info]) => ({
    area,
    label: info.label,
    prefix: info.prefix,
    points: info.points.map((p) => ({ ...p, area, draft: 0 })),
  }));
  const all = groups.flatMap((g) => g.points);

  const readable = (p) => p.enabled && (p.attr === "r" || p.attr === "rw");
  const writable = (p) => p.enabled && (p.attr === "w" || p.attr === "rw");
  const width = (p) => (p.type === "u32" ? 2 : 1);
  const numeric = all.filter((p) => readable(p) && p.type !== "bit");

  // ---- 趋势选择 + 限值告警（限值持久化） ----
  const savedLimits = JSON.parse(localStorage.getItem("gd32.limits") || "{}");
  let limits = savedLimits;
  // 默认趋势：优先闭环控制回路（设定值/反馈/输出），否则 ADC，最后任意两个
  let trendSel = [];
  {
    const prefer = ["LOOP_SP", "LOOP_FEEDBACK", "LOOP_OUTPUT"];
    trendSel = prefer.filter((n) => numeric.some((p) => p.name === n));
    if (trendSel.length === 0) trendSel = numeric.filter((p) => p.name.startsWith("ADC")).slice(0, 2).map((p) => p.name);
    if (trendSel.length === 0) trendSel = numeric.slice(0, 2).map((p) => p.name);
  }

  $: trendSeries = trendSel.map((name, i) => ({ name, color: PALETTE[i % PALETTE.length] }));

  // 各选中序列的 min/max（图例提示 + 归一化时有意义）
  $: stats = (() => {
    $revision;
    const m = {};
    for (const s of trendSeries) {
      const v = history.getSeries(s.name);
      if (!v.length) continue;
      let mn = v[0];
      let mx = v[0];
      for (const x of v) {
        if (x < mn) mn = x;
        if (x > mx) mx = x;
      }
      m[s.name] = { min: mn, max: mx };
    }
    return m;
  })();

  function toggleTrend(name) {
    trendSel = trendSel.includes(name)
      ? trendSel.filter((n) => n !== name)
      : [...trendSel, name];
  }

  function setLimit(name, which, raw) {
    const cur = { ...(limits[name] || {}) };
    if (raw === "" || raw === null) delete cur[which];
    else cur[which] = Number(raw);
    limits = { ...limits, [name]: cur };
    localStorage.setItem("gd32.limits", JSON.stringify(limits));
  }

  $: alarms = numeric.filter((p) => {
    const val = values[p.name];
    const l = limits[p.name];
    if (val === undefined || !l) return false;
    if (l.lo !== undefined && val < l.lo) return true;
    if (l.hi !== undefined && val > l.hi) return true;
    return false;
  });

  // 闭环控制回路的联锁状态（LOOP_ALARM bit0 = 反馈越限）
  $: loopAlarm = values["LOOP_ALARM"] ? 1 : 0;

  // 按区把连续地址合并成读取区间
  function buildLayout() {
    const byArea = {};
    for (const p of all) {
      if (!readable(p)) continue;
      (byArea[p.area] ||= []).push(p);
    }
    const layout = [];
    for (const area of Object.keys(byArea)) {
      const pts = byArea[area].sort((a, b) => a.addr - b.addr);
      let cur = null;
      for (const p of pts) {
        if (cur && cur.addr + cur.qty === p.addr) {
          cur.items.push({ name: p.name, offset: cur.qty, type: p.type });
          cur.qty += width(p);
        } else {
          cur = { area, addr: p.addr, qty: width(p), items: [{ name: p.name, offset: 0, type: p.type }] };
          layout.push(cur);
        }
      }
    }
    return layout;
  }
  const layout = buildLayout();
  const reqs = layout.map((c) => ({ area: c.area, addr: c.addr, qty: c.qty }));

  function decode(results) {
    const v = {};
    const e = {};
    for (let i = 0; i < layout.length; i++) {
      const c = layout[i];
      const r = results[i];
      for (const it of c.items) {
        if (r.error) {
          e[it.name] = r.error;
          continue;
        }
        const off = it.offset;
        if (it.type === "bit") v[it.name] = r.values[off] ? 1 : 0;
        else if (it.type === "s16") {
          const x = r.values[off];
          v[it.name] = x & 0x8000 ? x - 0x10000 : x;
        } else if (it.type === "u32") {
          v[it.name] = ((r.values[off] << 16) | r.values[off + 1]) >>> 0;
        } else v[it.name] = r.values[off];
      }
    }
    return { v, e };
  }

  // 值→显示串（响应式映射）。必须让模板直接依赖 values：
  // 若用 fmt(p)（values 在函数内部访问），Svelte 编译期追踪不到 values，
  // 值更新后不会重绘那一格（就会永远停在首帧的 “—”）。
  let disp = {};
  $: {
    const d = {};
    for (const p of all) {
      const val = values[p.name];
      d[p.name] = val === undefined ? "—" : (p.type === "bit" ? (val ? "1" : "0") : String(val));
    }
    disp = d;
  }

  async function refresh() {
    if (!connected) return;
    try {
      const results = await api.readRanges(reqs);
      const { v, e } = decode(results);
      values = v;
      errors = e;
      for (const p of numeric) {
        if (v[p.name] !== undefined) history.push(p.name, v[p.name]);
      }
      history.bump();
      const ok = results.filter((r) => !r.error).length;
      diag = `已读 ${ok}/${results.length} 区间`;
      if (ok !== results.length) {
        console.warn("[scada] 失败区间:", results.filter((r) => r.error).slice(0, 3));
      }
    } catch (err) {
      status = "读取失败: " + err;
      console.error("[scada] refresh failed", err);
    }
  }

  let timer = null;
  onMount(() => {
    api.isConnected().then((c) => (connected = c)).catch(() => {});
    timer = setInterval(refresh, pollMs);
    return () => clearInterval(timer);
  });

  async function doConnect() {
    try {
      await api.connect(host, port);
      connected = true;
      status = `已连接 ${host}:${port}`;
      await refresh();
    } catch (e) {
      connected = false;
      status = "" + e;
    }
  }

  async function doDisconnect() {
    await api.disconnect();
    connected = false;
    status = "未连接";
  }

  async function writePoint(p) {
    try {
      if (p.type === "bit") {
        await api.writeCoil(p.addr, !!p.draft);
      } else if (p.type === "u32") {
        const u = Number(p.draft) >>> 0;
        await api.writeReg(p.addr, (u >>> 16) & 0xffff);
        await api.writeReg(p.addr + 1, u & 0xffff);
      } else {
        await api.writeReg(p.addr, Number(p.draft) & 0xffff);
      }
      status = `写 ${p.name} = ${p.draft}`;
      await refresh();
    } catch (e) {
      status = "写入失败: " + e;
    }
  }

  async function toggleCoil(p) {
    try {
      // 以设备"当前值"为准：先回读一次，再写反值（避免用过期/缺失缓存导致总是写 1）
      let cur = values[p.name] ? 1 : 0;
      try {
        const r = await api.readRanges([{ area: p.area, addr: p.addr, qty: 1 }]);
        if (r[0] && !r[0].error) cur = r[0].values[0] ? 1 : 0;
      } catch (_) {
        /* 回读失败就退回缓存值 */
      }
      const next = cur ? 0 : 1;
      await api.writeCoil(p.addr, !!next);
      status = `翻转 ${p.name}: ${cur} → ${next}`;
      await refresh();
    } catch (e) {
      status = "翻转失败: " + e;
    }
  }

  function exportCsv() {
    if (!confirm("导出已缓存的历史数据为 CSV？")) return;
    const blob = new Blob([history.toCsv()], { type: "text/csv" });
    const a = document.createElement("a");
    a.href = URL.createObjectURL(blob);
    a.download = `gd32-scada-${Date.now()}.csv`;
    a.click();
    URL.revokeObjectURL(a.href);
  }

  // 同步可写点草稿初值
  let synced = false;
  $: if (connected && !synced && Object.keys(values).length) {
    for (const p of all) if (writable(p) && p.draft === 0 && values[p.name] !== undefined) p.draft = values[p.name];
    synced = true;
  }

  const colorOf = (name) => {
    const i = trendSel.indexOf(name);
    return i >= 0 ? PALETTE[i % PALETTE.length] : "#8ea1c4";
  };
</script>

<header style="display:flex;gap:12px;align-items:center;flex-wrap:wrap;margin-bottom:12px">
  <strong style="font-size:18px">GD32H759 SCADA</strong>
  <input bind:value={host} placeholder="host" style="width:160px" disabled={connected} />
  <input type="number" bind:value={port} style="width:80px" disabled={connected} />
  {#if connected}
    <button on:click={doDisconnect}>断开</button>
  {:else}
    <button on:click={doConnect}>连接</button>
  {/if}
  <button class="ghost" on:click={exportCsv}>导出 CSV</button>
  <button class="ghost" on:click={() => { history.clearAll(); }}>清空历史</button>
  <span style="color:var(--muted)">{status}</span>
  <span style="color:var(--muted)">{diag}</span>
  <span style="margin-left:auto;color:var(--muted)">
    {points.device} · {points.enabled_total}/{points.total} 点启用
  </span>
</header>

{#if loopAlarm}
  <div style="background:#3b1114;border:1px solid var(--bad);border-radius:8px;padding:8px 12px;margin-bottom:12px">
    <strong style="color:var(--bad)">控制回路联锁！</strong>
    反馈越限（LOOP_ALARM=1），已切安全输出{values["LOOP_FEEDBACK"] !== undefined ? `（反馈 ${values["LOOP_FEEDBACK"]}）` : ""}。
    检查 PA5↔PC2_C 接线 / 电位器负载，或写入 LOOP_RESET=1 复位。
  </div>
{/if}

{#if alarms.length}
  <div style="background:#3b1114;border:1px solid var(--bad);border-radius:8px;padding:8px 12px;margin-bottom:12px">
    <strong style="color:var(--bad)">告警 ({alarms.length})：</strong>
    {#each alarms as p (p.name)}
      <span style="margin-right:12px">
        {p.name} = {disp[p.name]}
        ({limits[p.name]?.lo !== undefined ? `低限 ${limits[p.name].lo}` : ""}
        {limits[p.name]?.hi !== undefined ? `高限 ${limits[p.name].hi}` : ""})
      </span>
    {/each}
  </div>
{/if}

<section style="background:var(--panel);border:1px solid var(--line);border-radius:10px;padding:10px 14px;margin-bottom:12px">
  <h3 style="margin:0 0 8px;font-size:15px;display:flex;align-items:center;gap:12px">
    趋势
    <label style="font-weight:400;font-size:12px;color:var(--muted)">
      <input type="checkbox" bind:checked={normalize} /> 各序列独立缩放
    </label>
  </h3>
  <div style="display:flex;gap:16px;align-items:flex-start">
    <div style="flex:1;min-width:0">
      <Trend series={trendSeries} height={200} normalize={normalize} />
    </div>
    <div style="width:280px;max-height:220px;overflow:auto">
      {#each numeric as p (p.name)}
        <div style="display:flex;align-items:center;gap:6px;margin-bottom:4px">
          <input type="checkbox" checked={trendSel.includes(p.name)} on:change={() => toggleTrend(p.name)} />
          <span style="flex:1;color:{colorOf(p.name)}"
                title={stats[p.name] ? `min ${stats[p.name].min} / max ${stats[p.name].max}` : ""}>{p.name}</span>
          <span style="color:var(--muted)">{values[p.name] ?? "—"}</span>
          <input
            type="number"
            placeholder="低"
            style="width:56px"
            value={limits[p.name]?.lo ?? ""}
            on:change={(e) => setLimit(p.name, "lo", e.target.value)}
          />
          <input
            type="number"
            placeholder="高"
            style="width:56px"
            value={limits[p.name]?.hi ?? ""}
            on:change={(e) => setLimit(p.name, "hi", e.target.value)}
          />
        </div>
      {/each}
    </div>
  </div>
</section>

{#each groups as g (g.area)}
  {#if g.points.some((p) => p.enabled)}
    <section style="background:var(--panel);border:1px solid var(--line);border-radius:10px;padding:10px 14px;margin-bottom:12px">
      <h3 style="margin:0 0 8px;font-size:15px">{g.label} <span style="color:var(--muted)">({g.prefix})</span></h3>
      <table>
        <thead>
          <tr><th>点</th><th>地址</th><th>类型</th><th>值</th><th>写</th></tr>
        </thead>
        <tbody>
          {#each g.points as p (g.area + p.name)}
            {#if p.enabled}
              <tr style={alarms.includes(p) ? "background:#3b1114" : ""}>
                <td>{p.name}</td>
                <td style="color:var(--muted)">{p.addr_hex}</td>
                <td style="color:var(--muted)">{p.type}/{p.attr}</td>
                <td>
                  <span>{disp[p.name]}</span>
                  {#if errors[p.name]}<span style="color:var(--bad)"> ⚠</span>{/if}
                </td>
                <td>
                  {#if writable(p)}
                    {#if p.type === "bit"}
                      <button class="ghost" on:click={() => toggleCoil(p)} disabled={!connected}>翻转</button>
                    {:else}
                      <span style="display:inline-flex;gap:6px">
                        <input type="number" bind:value={p.draft} disabled={!connected} />
                        <button on:click={() => writePoint(p)} disabled={!connected}>写</button>
                      </span>
                    {/if}
                  {/if}
                </td>
              </tr>
            {/if}
          {/each}
        </tbody>
      </table>
    </section>
  {/if}
{/each}
