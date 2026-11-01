import asyncio
import threading
from huoxing.worker import ImageWorker


def test_worker_bounds_background_tasks_after_cancel():
    started, release = threading.Event(), threading.Event()
    calls = []
    class SlowDetector:
        def ingest(self, data, **metadata):
            calls.append(data)
            started.set()
            release.wait(timeout=5)
            return "done"
    async def exercise():
        worker = ImageWorker(SlowDetector())
        waiter = asyncio.create_task(worker.check(b"first"))
        await asyncio.to_thread(started.wait, 2)
        waiter.cancel()
        try:
            await waiter
        except asyncio.CancelledError:
            pass
        assert await worker.check(b"second") is None
        release.set()
        await worker.close()
        assert calls == [b"first"]
    asyncio.run(exercise())
