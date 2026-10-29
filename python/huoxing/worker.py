"""宿主事件循环只保留一个在途任务，忙时跳过；取消等待不会启动更多后台任务。"""
import asyncio
from functools import partial


class ImageWorker:
    def __init__(self, detector):
        self.detector = detector
        self._task = None

    async def check(self, data, **metadata):
        return await self._run(partial(self.detector.ingest, data, **metadata))

    async def check_forward(self, segments, **metadata):
        from .forward import check_top_level
        return await self._run(partial(check_top_level, self.detector, segments, **metadata))

    async def _run(self, operation):
        if self._task is not None and not self._task.done():
            return None
        task = asyncio.create_task(asyncio.to_thread(operation))
        self._task = task
        # 即使宿主取消了等待，也检索异常，且等线程完成才接受下一个任务。
        task.add_done_callback(lambda done: done.exception() if not done.cancelled() else None)
        return await asyncio.shield(task)

    async def close(self):
        if self._task is not None:
            await asyncio.shield(self._task)


def format_hit(result):
    return (f"这张图之前出现过：消息 {result.first_message_id}，"
            f"首次记录 {result.first_seen}。依据：{result.evidence.method}。")
