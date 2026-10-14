/*
 * modbus.rs — 自包含 Modbus TCP 客户端（只依赖 tokio）
 *
 * 帧：MBAP[ tid:2 | pid:2=0 | len:2 | uid:1 ] + PDU
 * 支持 FC 0x01/0x02/0x03/0x04（读）与 0x05/0x06（写）。
 * 请求-响应一问一答，内部串行，故用一个 TcpStream 即可。
 */
use std::io::{Error, ErrorKind};
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::TcpStream;

pub struct Client {
    stream: TcpStream,
    tid: u16,
}

fn err(msg: impl Into<String>) -> Error {
    Error::new(ErrorKind::Other, msg.into())
}

impl Client {
    pub async fn connect(host: &str, port: u16) -> std::io::Result<Self> {
        let stream = TcpStream::connect((host, port)).await?;
        stream.set_nodelay(true).ok();
        Ok(Self { stream, tid: 0 })
    }

    /// 发一个请求 PDU，返回响应 PDU（异常响应转成 io::Error）
    async fn request(&mut self, pdu: &[u8]) -> std::io::Result<Vec<u8>> {
        self.tid = self.tid.wrapping_add(1);

        let mut frame = Vec::with_capacity(7 + pdu.len());
        frame.extend_from_slice(&self.tid.to_be_bytes());
        frame.extend_from_slice(&0u16.to_be_bytes());           // protocol id
        frame.extend_from_slice(&((pdu.len() + 1) as u16).to_be_bytes());
        frame.push(1u8);                                        // unit id
        frame.extend_from_slice(pdu);
        self.stream.write_all(&frame).await?;

        let mut head = [0u8; 6];                                // tid,pid,len
        self.stream.read_exact(&mut head).await?;
        let len = u16::from_be_bytes([head[4], head[5]]) as usize;
        if len < 2 {
            return Err(err("short MBAP length"));
        }
        let mut body = vec![0u8; len];                          // uid + pdu
        self.stream.read_exact(&mut body).await?;

        let resp = &body[1..];
        if resp.len() >= 2 && resp[0] & 0x80 != 0 {
            return Err(err(format!(
                "modbus exception fc=0x{:02X} code=0x{:02X}",
                resp[0] & 0x7F,
                resp[1]
            )));
        }
        Ok(resp.to_vec())
    }

    /// 读位：FC 0x01(线圈) / 0x02(离散输入)。返回每点 0/1。
    pub async fn read_bits(&mut self, fc: u8, addr: u16, qty: u16) -> std::io::Result<Vec<u16>> {
        let pdu = [fc, (addr >> 8) as u8, addr as u8, (qty >> 8) as u8, qty as u8];
        let resp = self.request(&pdu).await?;
        let bytes = (qty as usize + 7) / 8;
        // 严格校验 PDU = [fc, 字节数, 数据...]：长度/字节数必须与请求一致（防错位、防越界）
        if resp.len() != 2 + bytes || resp[0] != fc || resp[1] as usize != bytes {
            return Err(err(format!(
                "bad read-bits response: fc=0x{:02X} len={} bytecount={:?} expect_len={}",
                fc, resp.len(), resp.get(1), 2 + bytes
            )));
        }
        let mut out = Vec::with_capacity(qty as usize);
        for i in 0..qty as usize {
            let byte = resp[2 + i / 8];
            out.push(((byte >> (i % 8)) & 1) as u16);
        }
        Ok(out)
    }

    /// 读寄存器：FC 0x03(保持) / 0x04(输入)。返回原始 u16。
    pub async fn read_regs(&mut self, fc: u8, addr: u16, qty: u16) -> std::io::Result<Vec<u16>> {
        let pdu = [fc, (addr >> 8) as u8, addr as u8, (qty >> 8) as u8, qty as u8];
        let resp = self.request(&pdu).await?;
        let nbytes = qty as usize * 2;
        if resp.len() != 2 + nbytes || resp[0] != fc || resp[1] as usize != nbytes {
            return Err(err(format!(
                "bad read-regs response: fc=0x{:02X} len={} bytecount={:?} expect_len={}",
                fc, resp.len(), resp.get(1), 2 + nbytes
            )));
        }
        let mut out = Vec::with_capacity(qty as usize);
        for i in 0..qty as usize {
            out.push(u16::from_be_bytes([resp[2 + i * 2], resp[3 + i * 2]]));
        }
        Ok(out)
    }

    /// 写单寄存器：FC 0x06
    pub async fn write_reg(&mut self, addr: u16, val: u16) -> std::io::Result<()> {
        let pdu = [0x06, (addr >> 8) as u8, addr as u8, (val >> 8) as u8, val as u8];
        self.request(&pdu).await.map(|_| ())
    }

    /// 写单线圈：FC 0x05
    pub async fn write_coil(&mut self, addr: u16, on: bool) -> std::io::Result<()> {
        let v: u16 = if on { 0xFF00 } else { 0x0000 };
        let pdu = [0x05, (addr >> 8) as u8, addr as u8, (v >> 8) as u8, v as u8];
        self.request(&pdu).await.map(|_| ())
    }
}
