#include <sourcemod>
#include <profiler>
#include <json>

#pragma dynamic 531072
#define TEST_ITERATIONS 1000
#define SMALL_ITERATIONS 10000
#define LARGE_ITERATIONS 100000

Profiler g_hProfiler;

public Plugin myinfo = {
	name = "JSON Benchmark",
	author = "ProjectSky",
	description = "Performance benchmark for JSON",
	version = "1.0.0",
	url = "https://github.com/ProjectSky/sm-ext-json"
};

public void OnPluginStart()
{
	RegServerCmd("sm_json_benchmark", Command_Benchmark, "Run JSON performance benchmark");
	g_hProfiler = new Profiler();
}

Action Command_Benchmark(int args)
{
	PrintToServer("=== JSON Performance Benchmark ===");

	BenchmarkParsing();
	BenchmarkObjectOps();
	BenchmarkArrayOps();
	BenchmarkCloning();
	BenchmarkIteration();
	BenchmarkPointerOps();
	BenchmarkPatch();

	PrintToServer("=== Benchmark Complete ===");
	return Plugin_Handled;
}

void BenchmarkParsing()
{
	JSON json = JSON.Parse("twitter.json", true);
	int dataLength = json.ReadSize;
	char[] jsonStr = new char[dataLength];
	json.ToString(jsonStr, dataLength);

	PrintToServer("[Parse/Stringify] Data: %.2f MB", float(dataLength) / (1024.0 * 1024.0));

	g_hProfiler.Start();
	for (int i = 0; i < TEST_ITERATIONS; i++)
	{
		JSON testJson = JSON.Parse(jsonStr);
		delete testJson;
	}
	g_hProfiler.Stop();
	float parseTime = g_hProfiler.Time;

	g_hProfiler.Start();
	for (int i = 0; i < TEST_ITERATIONS; i++)
	{
		json.ToStringDirect(jsonStr, dataLength);
	}
	g_hProfiler.Stop();
	float stringifyTime = g_hProfiler.Time;

	PrintToServer("  Parse: %.2f ops/sec (%.2f MB/s)", TEST_ITERATIONS / parseTime, float(dataLength) * TEST_ITERATIONS / (parseTime * 1024.0 * 1024.0));
	PrintToServer("  Stringify: %.2f ops/sec (%.2f MB/s)", TEST_ITERATIONS / stringifyTime, float(dataLength) * TEST_ITERATIONS / (stringifyTime * 1024.0 * 1024.0));

	delete json;
}

void BenchmarkObjectOps()
{
	PrintToServer("\n[Object Operations] Iterations: %d", SMALL_ITERATIONS);

	g_hProfiler.Start();
	for (int i = 0; i < SMALL_ITERATIONS; i++)
	{
		JSONObject obj = new JSONObject();
		obj.SetInt("id", i);
		obj.SetString("name", "test");
		obj.SetFloat("value", 3.14);
		obj.SetBool("active", true);
		delete obj;
	}
	g_hProfiler.Stop();
	PrintToServer("  Create+Set: %.2f ops/sec", SMALL_ITERATIONS / g_hProfiler.Time);

	JSONObject obj = new JSONObject();
	for (int i = 0; i < 100; i++)
	{
		char key[16];
		key[0] = 'k'; key[1] = 'e'; key[2] = 'y';
		IntToString(i, key[3], sizeof(key) - 3);
		obj.SetInt(key, i);
	}

	g_hProfiler.Start();
	for (int i = 0; i < SMALL_ITERATIONS; i++)
	{
		obj.GetInt("key50");
	}
	g_hProfiler.Stop();
	PrintToServer("  Get: %.2f ops/sec", SMALL_ITERATIONS / g_hProfiler.Time);

	g_hProfiler.Start();
	for (int i = 0; i < SMALL_ITERATIONS; i++)
	{
		obj.HasKey("key50");
	}
	g_hProfiler.Stop();
	PrintToServer("  HasKey: %.2f ops/sec", SMALL_ITERATIONS / g_hProfiler.Time);

	delete obj;
}

void BenchmarkArrayOps()
{
	PrintToServer("\n[Array Operations] Iterations: %d", SMALL_ITERATIONS);

	g_hProfiler.Start();
	for (int i = 0; i < SMALL_ITERATIONS; i++)
	{
		JSONArray arr = new JSONArray();
		arr.PushInt(1);
		arr.PushString("test");
		arr.PushFloat(3.14);
		arr.PushBool(true);
		delete arr;
	}
	g_hProfiler.Stop();
	PrintToServer("  Create+Push: %.2f ops/sec", SMALL_ITERATIONS / g_hProfiler.Time);

	JSONArray arr = new JSONArray();
	for (int i = 0; i < 100; i++)
	{
		arr.PushInt(i);
	}

	g_hProfiler.Start();
	for (int i = 0; i < SMALL_ITERATIONS; i++)
	{
		arr.GetInt(50);
	}
	g_hProfiler.Stop();
	PrintToServer("  Get: %.2f ops/sec", SMALL_ITERATIONS / g_hProfiler.Time);

	g_hProfiler.Start();
	for (int i = 0; i < SMALL_ITERATIONS; i++)
	{
		arr.SetInt(50, i);
	}
	g_hProfiler.Stop();
	PrintToServer("  Set: %.2f ops/sec", SMALL_ITERATIONS / g_hProfiler.Time);

	delete arr;
}

void BenchmarkCloning()
{
	PrintToServer("\n[Clone Operations] Iterations: %d", TEST_ITERATIONS);

	JSONObject obj = new JSONObject();
	for (int i = 0; i < 50; i++)
	{
		char key[16];
		key[0] = 'k'; key[1] = 'e'; key[2] = 'y';
		IntToString(i, key[3], sizeof(key) - 3);
		obj.SetInt(key, i);
	}

	JSONObject target = new JSONObject();
	g_hProfiler.Start();
	for (int i = 0; i < TEST_ITERATIONS; i++)
	{
		target.Clear();
		JSON clone = JSON.DeepCopy(target, obj);
		delete clone;
	}
	g_hProfiler.Stop();
	PrintToServer("  DeepCopy: %.2f ops/sec", TEST_ITERATIONS / g_hProfiler.Time);

	delete obj;
	delete target;
}

void BenchmarkIteration()
{
	PrintToServer("\n[Iteration] Iterations: %d", TEST_ITERATIONS);

	JSONObject obj = new JSONObject();
	for (int i = 0; i < 100; i++)
	{
		char key[16];
		key[0] = 'k'; key[1] = 'e'; key[2] = 'y';
		IntToString(i, key[3], sizeof(key) - 3);
		obj.SetInt(key, i);
	}

	char key[32];
	JSONObjIter iterObj = new JSONObjIter(obj);
	g_hProfiler.Start();
	for (int i = 0; i < TEST_ITERATIONS; i++)
	{
		iterObj.Reset();
		while (iterObj.Next(key, sizeof(key)))
		{
			JSON val = iterObj.Value;
			delete val;
		}
	}
	g_hProfiler.Stop();
	PrintToServer("  Object: %.2f ops/sec", TEST_ITERATIONS / g_hProfiler.Time);

	JSONArray arr = new JSONArray();
	for (int i = 0; i < 100; i++)
	{
		arr.PushInt(i);
	}

	JSONArrIter iterArr = new JSONArrIter(arr);
	g_hProfiler.Start();
	for (int i = 0; i < TEST_ITERATIONS; i++)
	{
		JSON val;
		iterArr.Reset();
		while ((val = iterArr.Next) != null)
		{
			delete val;
		}
	}
	g_hProfiler.Stop();
	PrintToServer("  Array: %.2f ops/sec", TEST_ITERATIONS / g_hProfiler.Time);

	delete obj;
	delete arr;
	delete iterObj;
	delete iterArr;
}

void BenchmarkPointerOps()
{
	PrintToServer("\n[JSON Pointer] Iterations: %d", SMALL_ITERATIONS);

	JSONObject obj = new JSONObject();
	JSONObject nested = new JSONObject();
	nested.SetInt("value", 42);
	obj.Set("nested", nested);

	g_hProfiler.Start();
	for (int i = 0; i < SMALL_ITERATIONS; i++)
	{
		obj.PtrGetInt("/nested/value");
	}
	g_hProfiler.Stop();
	PrintToServer("  PtrGet: %.2f ops/sec", SMALL_ITERATIONS / g_hProfiler.Time);

	g_hProfiler.Start();
	for (int i = 0; i < SMALL_ITERATIONS; i++)
	{
		obj.PtrSetInt("/nested/value", i);
	}
	g_hProfiler.Stop();
	PrintToServer("  PtrSet: %.2f ops/sec", SMALL_ITERATIONS / g_hProfiler.Time);

	delete obj;
}

void BenchmarkPatch()
{
	PrintToServer("\n[JSON Patch] Iterations: %d", LARGE_ITERATIONS);

	JSONObject doc = new JSONObject();
	doc.SetInt("value", 42);

	JSONArray patch = new JSONArray();
	JSONObject op = new JSONObject();
	op.SetString("op", "replace");
	op.SetString("path", "/value");
	op.SetInt("value", 100);
	patch.Push(op);

	g_hProfiler.Start();
	for (int i = 0; i < LARGE_ITERATIONS; i++)
	{
		JSON result = doc.ApplyJsonPatch(patch);
		delete result;
	}
	g_hProfiler.Stop();
	PrintToServer("  ApplyPatch: %.2f ops/sec", LARGE_ITERATIONS / g_hProfiler.Time);

	JSONObject merge = new JSONObject();
	merge.SetInt("value", 200);

	g_hProfiler.Start();
	for (int i = 0; i < LARGE_ITERATIONS; i++)
	{
		JSON result = doc.ApplyMergePatch(merge);
		delete result;
	}
	g_hProfiler.Stop();
	PrintToServer("  MergePatch: %.2f ops/sec", LARGE_ITERATIONS / g_hProfiler.Time);

	delete doc;
	delete patch;
	delete merge;
}
