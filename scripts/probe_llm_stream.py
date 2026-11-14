"""One synthetic streaming POST. Never retries or prints credentials."""
import json
import pathlib
import sys
import time
import urllib.request

settings = json.loads(pathlib.Path(sys.argv[1]).read_text(encoding="utf-8-sig"))
out = pathlib.Path(sys.argv[2])
out.mkdir(parents=True, exist_ok=False)
body = {"model": settings["llmModel"], "stream": True,
        "messages": [{"role": "user", "content": 'Return only this JSON: {"ok":true}'}]}
(out / "request.json").write_text(json.dumps(body), encoding="utf-8")
request = urllib.request.Request(settings["llmBaseUrl"].rstrip("/") + "/chat/completions",
    data=json.dumps(body).encode(), headers={"Content-Type": "application/json",
    "Authorization": "Bearer " + settings["llmApiKey"]})
start = time.monotonic()
result = {"posts": 1, "done": False}
try:
    with urllib.request.urlopen(request, timeout=180) as response, (out / "response.sse").open("wb") as raw:
        result.update(status=response.status, content_type=response.headers.get("Content-Type"),
                      headers_seconds=round(time.monotonic() - start, 3))
        print(json.dumps(result), flush=True)
        for line in response:
            raw.write(line)
            raw.flush()
            if line.strip() == b"data: [DONE]":
                result["done"] = True
                break
except Exception as error:
    result.update(error=str(error), outcome_unknown=True)
finally:
    result["elapsed_seconds"] = round(time.monotonic() - start, 3)
    (out / "result.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
    print(json.dumps(result), flush=True)
sys.exit(0 if result["done"] else 1)
