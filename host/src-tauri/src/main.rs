/*
 * main.rs — Tauri 后端命令
 *
 * 前端按区提交一组连续区间读取请求，后端串行执行后一次性返回。
 * 区域字符串 -> 功能码：coil=0x01 din=0x02 hreg=0x03 ireg=0x04
 */
#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

mod modbus;

use modbus::Client;
use serde::{Deserialize, Serialize};
use tokio::sync::Mutex as AsyncMutex;

struct AppState {
    client: AsyncMutex<Option<Client>>,
}

#[derive(Debug, Deserialize)]
struct ReadRange {
    area: String,
    addr: u16,
    qty: u16,
}

#[derive(Debug, Serialize)]
struct RangeResult {
    area: String,
    addr: u16,
    values: Vec<u16>,
    error: Option<String>,
}

/// 点表区定义。**规范名必须与 tools/points.json 的 areas 键一致** —— 这是前后端的契约。
struct AreaDef {
    name: &'static str,               // 规范名（= points.json areas 键）
    aliases: &'static [&'static str], // 兼容别名
    fc: u8,                           // 读功能码
    bits: bool,                       // true=位区(线圈/离散输入)，false=寄存器区
}

/// 区表：单一事实来源。前端按区名提交请求，后端据此选功能码。
const AREAS: &[AreaDef] = &[
    AreaDef { name: "coil",             aliases: &[],       fc: 0x01, bits: true  },
    AreaDef { name: "discrete_input",   aliases: &["din"],  fc: 0x02, bits: true  },
    AreaDef { name: "input_register",   aliases: &["ireg"], fc: 0x04, bits: false },
    AreaDef { name: "holding_register", aliases: &["hreg"], fc: 0x03, bits: false },
];

fn find_area(area: &str) -> Option<&'static AreaDef> {
    AREAS.iter().find(|a| a.name == area || a.aliases.contains(&area))
}

#[tauri::command]
async fn connect(state: tauri::State<'_, AppState>, host: String, port: u16) -> Result<(), String> {
    let client = Client::connect(&host, port)
        .await
        .map_err(|e| format!("连接失败: {e}"))?;
    *state.client.lock().await = Some(client);
    Ok(())
}

#[tauri::command]
async fn disconnect(state: tauri::State<'_, AppState>) -> Result<(), String> {
    // 关闭连接 = 丢弃 TcpStream，由 Drop 关闭
    *state.client.lock().await = None;
    Ok(())
}

#[tauri::command]
async fn is_connected(state: tauri::State<'_, AppState>) -> Result<bool, String> {
    Ok(state.client.lock().await.is_some())
}

#[tauri::command]
async fn read_ranges(
    state: tauri::State<'_, AppState>,
    reqs: Vec<ReadRange>,
) -> Result<Vec<RangeResult>, String> {
    let mut guard = state.client.lock().await;
    let client = guard.as_mut().ok_or("未连接")?;

    let mut out = Vec::with_capacity(reqs.len());
    for r in reqs {
        let def = match find_area(&r.area) {
            Some(d) => d,
            None => {
                let msg = format!("未知区: {}", r.area);
                out.push(RangeResult { area: r.area, addr: r.addr, values: vec![], error: Some(msg) });
                continue;
            }
        };
        let res = if def.bits {
            client.read_bits(def.fc, r.addr, r.qty).await
        } else {
            client.read_regs(def.fc, r.addr, r.qty).await
        };
        match res {
            Ok(values) => out.push(RangeResult { area: r.area, addr: r.addr, values, error: None }),
            Err(e) => out.push(RangeResult { area: r.area, addr: r.addr, values: vec![], error: Some(e.to_string()) }),
        }
    }
    Ok(out)
}

#[tauri::command]
async fn write_reg(state: tauri::State<'_, AppState>, addr: u16, value: u16) -> Result<(), String> {
    let mut guard = state.client.lock().await;
    let client = guard.as_mut().ok_or("未连接")?;
    client.write_reg(addr, value).await.map_err(|e| e.to_string())
}

#[tauri::command]
async fn write_coil(state: tauri::State<'_, AppState>, addr: u16, on: bool) -> Result<(), String> {
    let mut guard = state.client.lock().await;
    let client = guard.as_mut().ok_or("未连接")?;
    client.write_coil(addr, on).await.map_err(|e| e.to_string())
}

fn main() {
    tauri::Builder::default()
        .manage(AppState {
            client: AsyncMutex::new(None),
        })
        .invoke_handler(tauri::generate_handler![
            connect,
            disconnect,
            is_connected,
            read_ranges,
            write_reg,
            write_coil
        ])
        .run(tauri::generate_context!())
        .expect("error while running tauri application");
}

#[cfg(test)]
mod tests {
    use super::*;

    /// 锁住"区名 → 功能码"契约：规范名必须与 tools/points.json 的 areas 键一致
    #[test]
    fn area_lookup_matches_points_json() {
        assert_eq!(find_area("coil").unwrap().fc, 0x01);
        assert_eq!(find_area("discrete_input").unwrap().fc, 0x02);
        assert_eq!(find_area("input_register").unwrap().fc, 0x04);
        assert_eq!(find_area("holding_register").unwrap().fc, 0x03);
        // 别名仍可用
        assert_eq!(find_area("din").unwrap().fc, 0x02);
        assert_eq!(find_area("hreg").unwrap().fc, 0x03);
        // 位区/寄存器区分类正确
        assert!(find_area("coil").unwrap().bits);
        assert!(!find_area("holding_register").unwrap().bits);
        // 未知区返回 None
        assert!(find_area("bogus").is_none());
    }
}
