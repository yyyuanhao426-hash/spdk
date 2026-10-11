SPDK URMA 特性代码 review
审查对象：yyyuanhao426-hash/spdk 的 urma_enable，固定 HEAD ab0dce645c9a4ed8562df11ab99fb85fc6286b2f。比较基线为该仓库 master 的 3230130e6a4114a97991d7e27cecd5ab64b696c3，也是共同祖先；共 65 个提交、36 个变更文件。重点检查 initiator、target、共享设备/内存层、公共 iobuf 改动及通用 NVMe/NVMf 生命周期调用链；没有验证性能工具和脚本。

1. [P1] Target 断连会释放仍被 transport 使用的请求和 buffer
定位：lib/nvmf/urma.c:2011–2022。

nvmf_urma_qpair_fini() 先遍历 working_reqs 调用 nvmf_urma_release_req()，归还 iobuf/注销非缓存 segment，然后才删除 jetty，最终直接释放请求数组和 qpair。这里没有等待在途 URMA WR、撤销等待 buffer 的回调，也没有等待已发送到 owner thread 的 completion message。

不能依赖上层替它排空：spdk_nvmf_qpair_disconnect() 只等待通用 qpair->outstanding；写数据的 URMA READ 在 spdk_nvmf_request_exec() 之前发出，此时请求仅在 transport 的 working_reqs 中。连接在 PULLING 或 NEED_BUFFER 阶段断开即可进入提前释放路径。另一个 poll group 已取出的 CR 及转发消息仍保存裸 user_ctx，删除硬件队列无法撤销这些软件引用。

后果：可能向已归还的 iobuf 写入数据，或在后续 CR/消息/buffer 回调中访问已释放的请求和 qpair。

建议：建立异步 drain/fini 状态机，停止接收、取消等待 buffer 的请求、排空/flush 在途 WR，并计数已转发的完成消息；所有引用消失后再归还 buffer、销毁 segment 和 qpair。

2. [P1] 写请求在 URMA 数据搬运失败时返回 CID=0，可能完成另一条请求
定位：lib/nvmf/urma.c:1547–1554，以及 CR 错误处理:1764–1778。

接收命令时 ureq->rsp 被清零。CID/SQID 只在 nvmf_urma_req_complete() 中赋值，而 H2C 的 import/register/post 失败和 PULLING CR 错误直接调用 nvmf_urma_send_response()，绕过该初始化，因此返回的 CID/SQID 仍然是 0。

例如 CID=7 的写请求遇到 post 失败：initiator 会按响应 CID=0 查找 outstanding 请求。若 CID=0 仍在途，就错误地完成它并提前释放其资源；若不存在，返回协议错误；原 CID=7 请求仍未完成。

建议：接收命令时即初始化完成报文的身份字段，或把 CID/SQID/SQHD 填充收敛到所有响应路径都会经过的函数。

3. [P1] Target 在 accept poller 内阻塞读取握手，单个未发送 HELLO 的连接可卡住线程
定位：lib/nvmf/urma.c:792，820–822，511–524。

accept4() 仅设置 SOCK_CLOEXEC，返回的连接是阻塞 socket；监听 socket 的 nonblocking 属性不会在 Linux 上自动继承。随后 accept poller 同步调用 handshake，read_full() 使用阻塞 recv(..., 0)，没有握手期限。

触发只需连接监听端口但不发完整 HELLO；这既可能来自异常客户端，也可能来自普通连接建立后的进程停顿。该 SPDK 线程会一直停在 recv，不能继续接收其他连接或执行同线程工作。默认 TCP capsule 模式的阻塞 write_full() 在对端停止读取、发送缓冲耗尽时也会阻塞 poll group。

建议：采用非阻塞 socket 和可增量推进的接收/发送状态机，处理短读、短写、EAGAIN，并设置握手期限。

4. [P1] 重连覆盖旧 URMA 资源，遗留接收队列和设备引用
定位：lib/nvme/nvme_urma.c:1295–1305，1259–1278。

disconnect 仅 abort 请求、关闭 socket 并设置 DISCONNECTED，没有回收 jettys、target_jettys、JFR、capsule 接收资源或 device 引用。通用 spdk_nvme_ctrlr_reconnect_io_qpair() 在同一个 qpair 上再次调用 transport connect；后者重新 open device、calloc jetty 数组和接收资源，直接覆盖旧指针。

反复断连/重连会泄漏 context 引用和硬件队列；URMA capsule 模式还会对未销毁的 mutex 再次 init，保留旧 pending response 计数，并让旧 RX slot 继续引用已重连的 qpair，存在旧响应混入新连接的风险。

建议：让每一轮连接拥有独立且可排空的 transport 状态；完成旧连接 teardown 后才能重建。CID 位图等 qpair 级资源应与连接级资源分开管理，不能简单调用当前会释放位图的 release 函数后直接重连。

5. [P1] 共享 JFC 的错误被归给当前 poller，且错误后的已取出完成记录被丢弃
定位：lib/nvme/nvme_urma.c:969–974，1034–1054。

每个 qpair 都会轮询设备级共享 JFC，所以 A 可以取到 B 的完成记录。成功 RX 会转发给 owner，但 cr->status != URMA_CR_SUCCESS 时尚未解析 owner 就直接返回 -EIO。结果 A 的 completions 调用报错，而 B 的错误记录已经消耗，B 仍然等待完成。

此外，一次 poll 可取出一批记录；处理其中一条失败便直接 return，后面记录既不处理也不暂存，可能连带丢失其他 qpair 的成功完成。

建议：依据 user_ctx 将错误送到所属 qpair 的线程/错误状态，并处理或保存本批全部已取出的 CR，避免错误传播到无关连接。

6. [P2] Initiator 的 URMA capsule 模式没有检查 lifetime socket 关闭
定位：lib/nvme/nvme_urma.c:1070–1076。

URMA capsule 分支只 poll JFC；TCP EOF 检测仅在 TCP capsule 分支。Target 已实现 nvmf_urma_check_lifetime_socket()，initiator 没有对应逻辑，也没有在此 transport 中处理能替代它的异步连接故障事件。

若 target 在收到命令后、发送 response 前退出，且 initiator 的 SEND 已成功完成，就不保证再有一个 JFC 错误通知退出；此时 socket 已 EOF，但 initiator 仍可持续返回 0 completions，无法由这条路径触发断连恢复。

建议：URMA 模式也检查 bootstrap/lifetime socket 的 EOF/错误，并把故障记入 qpair 状态、完成 outstanding 请求。

7. [P2] 大 buffer 配置使公共 iobuf chunk 元数据数组越界
定位：lib/thread/iobuf.c:157–166，194–196。

表容量按 ceil(large_pool_count / 4) 分配，但受 256 MiB chunk 字节上限影响，初始 chunk_bufs 可以为 1、2 或 3。合法配置 large_pool_count=8, large_bufsize=128 MiB 会分配 2 项元数据，而实际需要 4 个 chunk；第 3 次写 large_pool_chunks[allocated++] 即越界。opts 检查只有最小 bufsize 限制，未排除此配置。

这是公共 iobuf 路径的改动，不需要启用 URMA 才受影响。

建议：按实际可能的最小 chunk buffer 数计算表容量，或按需扩容；补充字节上限导致 chunk 小于 4 的边界测试。

8. [P2] 动态库没有导出 target 需要的 registry 函数
定位：lib/nvme/spdk_nvme.map:11–12。

lib/nvmf/urma.c:898,1459 跨库调用 nvme_urma_region_registry_add() 和 nvme_urma_region_registry_find()；它们定义在编入 libspdk_nvme 的 nvme_urma_common.c。新增导出规则只有 spdk_nvme_urma_* 和 spdk_urma_*，均不匹配以 nvme_urma_ 开头的这两个符号；map 末尾又有 local: *;。

mk/spdk.common.mk:481–486 构建 shared library 时使用该 version script。因此启用 URMA 的 shared 配置无法通过 libspdk_nvme 动态符号表解析这两个 target 依赖，可能在应用链接或加载阶段失败；静态链接不会暴露这个问题。

建议：显式导出所需内部跨库接口，或按项目规范改名/调整公共实现归属，并补充 URMA + shared build 检查。
