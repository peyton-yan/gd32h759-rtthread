<script>
  import { getSeries, revision } from "./history.js";

  // series: [{ name, color }]
  export let series = [];
  export let height = 200;
  // true = 每条序列按自身 min/max 独立缩放到全高（不同量程也能看清）
  // false = 所有序列共用同一 Y 轴
  export let normalize = false;

  let canvas;
  let box;
  let width = 720;

  function rangeOf(v) {
    let mn = Infinity;
    let mx = -Infinity;
    for (const x of v) {
      if (x < mn) mn = x;
      if (x > mx) mx = x;
    }
    if (!isFinite(mn)) return null;
    if (mn === mx) {
      mn -= 1;
      mx += 1;
    }
    return [mn, mx];
  }

  function draw() {
    if (!canvas || !box) return;
    const dpr = window.devicePixelRatio || 1;
    const w = box.clientWidth || width;
    const h = height;
    canvas.width = w * dpr;
    canvas.height = h * dpr;
    const ctx = canvas.getContext("2d");
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    ctx.clearRect(0, 0, w, h);

    // 背景网格
    ctx.strokeStyle = "#22304e";
    ctx.lineWidth = 1;
    for (let i = 0; i <= 4; i++) {
      const y = 6 + ((h - 12) * i) / 4;
      ctx.beginPath();
      ctx.moveTo(36, y);
      ctx.lineTo(w - 6, y);
      ctx.stroke();
    }

    const items = series.map((s) => {
      const v = getSeries(s.name);
      return { s, v, r: rangeOf(v) };
    });

    // 全局值域（未归一化时的 Y 轴）
    let gmin = Infinity;
    let gmax = -Infinity;
    for (const it of items) {
      if (!it.r) continue;
      if (it.r[0] < gmin) gmin = it.r[0];
      if (it.r[1] > gmax) gmax = it.r[1];
    }
    if (!isFinite(gmin)) {
      gmin = 0;
      gmax = 1;
    }
    if (gmin === gmax) {
      gmin -= 1;
      gmax += 1;
    }

    const x0 = 40;
    const x1 = w - 8;
    const y0 = 8;
    const y1 = h - 16;

    // 轴标注
    ctx.fillStyle = "#8ea1c4";
    ctx.font = "10px monospace";
    if (normalize) {
      ctx.fillText("norm", 2, y0 + 8);
    } else {
      ctx.fillText(gmax.toFixed(0), 4, y0 + 8);
      ctx.fillText(gmin.toFixed(0), 4, y1);
    }

    for (const it of items) {
      const { v, r } = it;
      if (!r || v.length < 2) continue;
      const mn = normalize ? r[0] : gmin;
      const mx = normalize ? r[1] : gmax;
      const scale = (val) => y1 - ((y1 - y0) * (val - mn)) / (mx - mn);

      ctx.beginPath();
      ctx.strokeStyle = it.s.color;
      ctx.lineWidth = 1.6;
      for (let i = 0; i < v.length; i++) {
        const x = x0 + ((x1 - x0) * i) / (v.length - 1);
        const y = scale(v[i]);
        if (i === 0) ctx.moveTo(x, y);
        else ctx.lineTo(x, y);
      }
      ctx.stroke();

      // 最新值端点
      ctx.fillStyle = it.s.color;
      ctx.fillRect(x1 - 2, scale(v[v.length - 1]) - 2, 4, 4);
    }
  }

  // 依赖：$revision（新数据）/ 宽度 / series / normalize 变化时重绘
  $: if (canvas && box) {
    $revision;
    series;
    normalize;
    draw();
  }
</script>

<div
  bind:this={box}
  bind:clientWidth={width}
  style="width:100%;background:#0b1220;border:1px solid var(--line);border-radius:8px"
>
  <canvas bind:this={canvas} style="width:100%;height:{height}px;display:block"></canvas>
</div>
