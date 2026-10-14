// 生成一个 32x32 纯色 ICO（无第三方依赖），供 tauri-build Windows 资源使用
import { writeFileSync, mkdirSync } from "node:fs";

const W = 32, H = 32;
const header = Buffer.alloc(6);
header.writeUInt16LE(0, 0); // reserved
header.writeUInt16LE(1, 2); // type = icon
header.writeUInt16LE(1, 4); // count

const bmpHeader = Buffer.alloc(40);
bmpHeader.writeUInt32LE(40, 0);
bmpHeader.writeInt32LE(W, 4);
bmpHeader.writeInt32LE(H * 2, 8); // XOR + AND
bmpHeader.writeUInt16LE(1, 12);
bmpHeader.writeUInt16LE(32, 14);
bmpHeader.writeUInt32LE(0, 16);

const pixels = Buffer.alloc(W * H * 4);
for (let i = 0; i < W * H; i++) {
  // BGRA: #38bdf8
  pixels[i * 4] = 0xf8;
  pixels[i * 4 + 1] = 0xbd;
  pixels[i * 4 + 2] = 0x38;
  pixels[i * 4 + 3] = 0xff;
}
const andMask = Buffer.alloc(((W + 31) / 32) * 4 * H); // 全 0 = 不透明
bmpHeader.writeUInt32LE(pixels.length + andMask.length, 20);
const bmp = Buffer.concat([bmpHeader, pixels, andMask]);

const dir = Buffer.alloc(16);
dir.writeUInt8(W, 0);
dir.writeUInt8(H, 1);
dir.writeUInt8(0, 2);
dir.writeUInt8(0, 3);
dir.writeUInt16LE(1, 4);
dir.writeUInt16LE(32, 6);
dir.writeUInt32LE(bmp.length, 8);
dir.writeUInt32LE(6 + 16, 12);

mkdirSync("icons", { recursive: true });
const ico = Buffer.concat([header, dir, bmp]);
writeFileSync("icons/icon.ico", ico);
console.log("wrote icons/icon.ico", ico.length, "bytes");
