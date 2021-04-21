import websockets as ws
import asyncio
from time import sleep

async def hello():
    async with ws.connect("ws://192.168.100.185/ws") as websocket:
        result = await websocket.recv()
        print(f"< {result}")
        sleep(10)

asyncio.get_event_loop().run_until_complete(hello())
