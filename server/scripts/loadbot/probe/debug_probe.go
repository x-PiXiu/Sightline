// debug probe：连接服务器，hexdump 前 32 字节，验证 WELCOME 帧格式
package main

import (
	"encoding/hex"
	"fmt"
	"net"
	"time"
)

func main() {
	c, err := net.DialTimeout("tcp", "127.0.0.1:18889", 3*time.Second)
	if err != nil {
		fmt.Println("dial:", err)
		return
	}
	defer c.Close()
	c.SetReadDeadline(time.Now().Add(3 * time.Second))
	buf := make([]byte, 32)
	n, err := c.Read(buf)
	fmt.Printf("read n=%d err=%v\nraw=%s\n", n, err, hex.EncodeToString(buf[:n]))
}
