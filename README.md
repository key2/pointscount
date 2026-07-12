# pointscount

A live TikTok gift-point counter. It connects to a TikTok LIVE room (via
[ttlive-cpp](https://github.com/key2/ttlive-cpp), included as a git
submodule), starts from the number of points already on the stream, and adds
the diamond value of every gift received from that moment on, printing the
running total.

```
[CONNECTED] @sandrahensley7197 room_id=7661584248248552208  starting from 0 points
[STREAK…] crococrodile sending 'Rose' x2 (worth 2 so far, not counted yet)
>>> TOTAL POINTS: 0  (+2 streaking)
[GIFT] crococrodile sent 'Rose' x5 = +5 points
>>> TOTAL POINTS: 5
```

**Correct streak handling.** While a streakable gift combo is in progress,
TikTok sends intermediate gift messages whose `repeat_count` is a *running
value* for the same combo — counting those would inflate the total (a Rose x5
streak arrives as x1, x2, x5). pointscount shows them as a live "pending"
figure only, and commits the combo's real value (`repeat_count ×
diamond_count`) exactly once, when the final message of the streak arrives.
Non-streakable gifts are committed immediately. Concurrent streaks are tracked
independently per (sender, gift).

## Building

**Prerequisites**

- C++17 compiler, CMake ≥ 3.16
- OpenSSL, protobuf (libprotobuf + protoc), zlib
- git, and internet access on first configure (CMake downloads a prebuilt
  [curl-impersonate](https://github.com/lexiforest/curl-impersonate))

**Linux / macOS**

```sh
git clone --recurse-submodules https://github.com/key2/pointscount.git
cd pointscount
# If you cloned without --recurse-submodules:
git submodule update --init --recursive

cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

**Windows** (use vcpkg for OpenSSL/protobuf/zlib)

```powershell
git clone --recurse-submodules https://github.com/key2/pointscount.git
cd pointscount
vcpkg install openssl protobuf zlib
cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release
```

## Running

```sh
./build/pointscount <@username> [options]
```

| Option | Meaning |
|---|---|
| `--start <points>` | Points already on the stream when you start counting (default 0) |
| `--room-id <id>` | Skip room scraping, connect directly to a room id |
| `--no-live-check` | Skip the is-live check |
| `--cookies "k=v;.."` | Seed cookies (e.g. ttwid / sessionid) |
| `--no-ws` | Use HTTP long-polling instead of the WebSocket |

Example — the stream currently shows 15 000 points and you want to keep
counting from there:

```sh
./build/pointscount @sandrahensley7197 --start 15000
```

Press `Ctrl+C` to stop; the final total is printed on exit. Gifts received
before the program connects are not counted (the connect backlog is skipped),
so the baseline you pass with `--start` stays correct.

---

# pointscount（中文说明）

一个 TikTok 直播礼物积分实时计数器。它通过
[ttlive-cpp](https://github.com/key2/ttlive-cpp)（以 git 子模块方式引入）
连接到 TikTok 直播间，从直播间当前已有的积分数开始，累加此后收到的每个
礼物的钻石价值，并实时打印总积分。

**正确处理连击（streak）。** 当可连击礼物的连击进行中时，TikTok 会发送
中间礼物消息，其中的 `repeat_count` 是同一次连击的*累计值*——如果直接累加
这些中间值，总数就会虚高（一次 Rose x5 连击会依次收到 x1、x2、x5）。
pointscount 只把它们作为"进行中"的临时数值显示，等连击的最后一条消息到达
时，才把该次连击的真实价值（`repeat_count × diamond_count`）一次性计入总分。
不可连击的礼物则立即计入。不同用户/不同礼物的并发连击互不干扰。

## 编译

**依赖**

- 支持 C++17 的编译器，CMake ≥ 3.16
- OpenSSL、protobuf（libprotobuf + protoc）、zlib
- git；首次配置时需要联网（CMake 会下载预编译的
  [curl-impersonate](https://github.com/lexiforest/curl-impersonate)）

**Linux / macOS**

```sh
git clone --recurse-submodules https://github.com/key2/pointscount.git
cd pointscount
# 如果克隆时没有加 --recurse-submodules：
git submodule update --init --recursive

cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

**Windows**（用 vcpkg 安装 OpenSSL/protobuf/zlib）

```powershell
git clone --recurse-submodules https://github.com/key2/pointscount.git
cd pointscount
vcpkg install openssl protobuf zlib
cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release
```

## 运行

```sh
./build/pointscount <@用户名> [选项]
```

| 选项 | 含义 |
|---|---|
| `--start <积分>` | 开始计数时直播间已有的积分数（默认 0） |
| `--room-id <id>` | 跳过主页解析，直接用房间 id 连接 |
| `--no-live-check` | 跳过开播状态检查 |
| `--cookies "k=v;.."` | 预置 Cookie（例如 ttwid / sessionid） |
| `--no-ws` | 用 HTTP 长轮询代替 WebSocket |

示例——直播间当前显示 15000 积分，从这个数继续计数：

```sh
./build/pointscount @sandrahensley7197 --start 15000
```

按 `Ctrl+C` 停止，退出时会打印最终总分。程序连接之前收到的礼物不会被计入
（连接时的历史消息会被跳过），因此 `--start` 传入的基准值保持准确。

## License / 许可

For interoperability/research. Respect TikTok's Terms of Service and
applicable laws. / 仅用于互操作性与研究目的，请遵守 TikTok 服务条款及相关
法律法规。
