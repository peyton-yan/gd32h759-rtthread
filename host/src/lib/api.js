import { invoke } from "@tauri-apps/api/core";

export const connect = (host, port) => invoke("connect", { host, port });
export const disconnect = () => invoke("disconnect");
export const isConnected = () => invoke("is_connected");
export const readRanges = (reqs) => invoke("read_ranges", { reqs });
export const writeReg = (addr, value) => invoke("write_reg", { addr, value });
export const writeCoil = (addr, on) => invoke("write_coil", { addr, on });
