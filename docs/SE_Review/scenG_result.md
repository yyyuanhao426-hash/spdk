# 场景 G — iobuf 元数据越界单测（SE_Review #7）

**日期**：2026-10-11
**验证对象**：0278f32f3 提交 —— iobuf 表容量按实际 chunk 数计算 + 边界单测

## 结论

**PASS** — `test/unit/lib/thread/iobuf.c/iobuf_ut` 全部通过（5 tests / 141 asserts），含新增边界测试 `iobuf_large_chunk_table_boundary`。

## 方法与结果

node1 ASAN worktree（`/home/yin/nof/spdk_asan`，HEAD fba60e72e，含 #7 修复）运行：

```
Suite: io_channel
  Test: iobuf_pool_memory ...passed
  Test: iobuf ...passed
  Test: iobuf_cache ...passed
  Test: iobuf_priority ...passed
  Test: iobuf_large_chunk_table_boundary ...passed

Run Summary:    Type  Total  Ran  Passed  Failed  Inactive
              suites      1    1    n/a       0        0
               tests      5    5      5       0        0
             asserts    141  141    141       0      n/a
```

边界测试覆盖：`large_pool_count=4, large_bufsize=64MB+4096`（大于 256MiB chunk 上限的整除边界），验证 chunk 表容量按实际 chunk 数分配，不再越界。
