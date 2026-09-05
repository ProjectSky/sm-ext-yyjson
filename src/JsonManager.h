#pragma once

#include <IJsonManager.h>
#include <yyjson.h>
#include "RefCounted.h"
#include <random>
#include <memory>
#include <charconv>
#include <cstdio>
#include <cstdarg>

/**
 * @brief Unified helper for native error buffers
 */
class NativeErrorBuffer {
private:
	static void SetV(char* error, size_t error_size, const char* format, va_list args) {
		if (!error || error_size == 0 || !format) {
			return;
		}

		int needed = std::vsnprintf(error, error_size, format, args);
		if (needed < 0 || static_cast<size_t>(needed) >= error_size) {
			error[error_size - 1] = '\0';
		}
	}

public:
	char* buffer{ nullptr };
	size_t size{ 0 };

	static void Clear(char* error, size_t error_size) {
		if (error && error_size > 0) {
			error[0] = '\0';
		}
	}

	static void Set(char* error, size_t error_size, const char* format, ...) {
		va_list args;
		va_start(args, format);
		SetV(error, error_size, format, args);
		va_end(args);
	}

	void Set(const char* format, ...) const {
		va_list args;
		va_start(args, format);
		SetV(buffer, size, format, args);
		va_end(args);
	}

	explicit operator bool() const {
		return buffer != nullptr && size > 0;
	}

	bool TryReadNonNegativeIndex(cell_t index_param, size_t* out_index) const {
		if (!out_index) {
			return false;
		}

		if (index_param < 0) {
			Set("Index must be >= 0 (got %d)", index_param);
			return false;
		}

		*out_index = static_cast<size_t>(index_param);
		return true;
	}

	static bool TryReadNonNegativeOrThrow(SourcePawn::IPluginContext* pContext,
		const char* field_name, cell_t value_param, size_t* out_value)
	{
		if (!pContext || !out_value || !field_name) {
			return false;
		}

		if (value_param < 0) {
			pContext->ThrowNativeError("%s must be >= 0 (got %d)", field_name, value_param);
			return false;
		}

		*out_value = static_cast<size_t>(value_param);
		return true;
	}

	static bool TryReadNonNegativeIndexOrThrow(SourcePawn::IPluginContext* pContext,
		cell_t index_param, size_t* out_index)
	{
		return TryReadNonNegativeOrThrow(pContext, "Index", index_param, out_index);
	}

	static NativeErrorBuffer FromPluginContext(SourcePawn::IPluginContext* pContext,
		const cell_t* params, int buffer_param, int size_param)
	{
		NativeErrorBuffer ref;
		if (!pContext || !params || buffer_param <= 0 || size_param <= 0) {
			return ref;
		}

		if (params[size_param] <= 0) {
			return ref;
		}

		pContext->LocalToString(params[buffer_param], &ref.buffer);
		ref.size = static_cast<size_t>(params[size_param]);
		NativeErrorBuffer::Clear(ref.buffer, ref.size);
		return ref;
	}
};

/**
 * @brief Wrapper for yyjson_mut_doc with intrusive reference counting
 */
class RefCountedMutDoc : public RefCounted {
private:
	yyjson_mut_doc *doc_;

public:
	explicit RefCountedMutDoc(yyjson_mut_doc *doc) noexcept : doc_(doc) {}

	RefCountedMutDoc(const RefCountedMutDoc &) = delete;
	RefCountedMutDoc &operator=(const RefCountedMutDoc &) = delete;

	~RefCountedMutDoc() noexcept override {
		if (doc_) {
			yyjson_mut_doc_free(doc_);
		}
	}

	yyjson_mut_doc *get() const noexcept { return doc_; }
};

/**
 * @brief Wrapper for yyjson_doc with intrusive reference counting
 */
class RefCountedImmutableDoc : public RefCounted {
private:
	yyjson_doc *doc_;

public:
	explicit RefCountedImmutableDoc(yyjson_doc *doc) noexcept : doc_(doc) {}

	RefCountedImmutableDoc(const RefCountedImmutableDoc &) = delete;
	RefCountedImmutableDoc &operator=(const RefCountedImmutableDoc &) = delete;

	~RefCountedImmutableDoc() noexcept override {
		if (doc_) {
			yyjson_doc_free(doc_);
		}
	}

	yyjson_doc *get() const noexcept { return doc_; }
};

/**
 * @brief JSON value wrapper
 *
 * Wraps json mutable/immutable documents and values.
 * Used as the primary data type for JSON operations.
 */
class JsonValue {
public:
	JsonValue() = default;
	~JsonValue() = default;

	JsonValue(const JsonValue&) = delete;
	JsonValue& operator=(const JsonValue&) = delete;

	void ResetObjectIterator() {
		m_iterInitialized = false;
	}

	void ResetArrayIterator() {
		m_iterInitialized = false;
		m_arrayIndex = 0;
	}

	bool IsMutable() const {
		return m_pDocument_mut != nullptr;
	}

	bool IsImmutable() const {
		return m_pDocument != nullptr;
	}

	size_t GetDocumentRefCount() const {
		if (m_pDocument_mut) {
			return m_pDocument_mut.use_count();
		} else if (m_pDocument) {
			return m_pDocument.use_count();
		}
		return 0;
	}

	// Mutable document
	RefPtr<RefCountedMutDoc> m_pDocument_mut;
	yyjson_mut_val* m_pVal_mut{ nullptr };

	// Immutable document
	RefPtr<RefCountedImmutableDoc> m_pDocument;
	yyjson_val* m_pVal{ nullptr };

	// Mutable document iterators
	yyjson_mut_obj_iter m_iterObj;
	yyjson_mut_arr_iter m_iterArr;

	// Immutable document iterators
	yyjson_obj_iter m_iterObjImm;
	yyjson_arr_iter m_iterArrImm;

	SourceMod::Handle_t m_handle{ BAD_HANDLE };
	size_t m_arrayIndex{ 0 };
	size_t m_readSize{ 0 };
	bool m_iterInitialized{ false };
};

/**
 * @brief Array iterator wrapper
 *
 * Wraps yyjson_arr_iter and yyjson_mut_arr_iter for array iteration.
 */
class JsonArrIter {
public:
	JsonArrIter() = default;
	~JsonArrIter() = default;

	JsonArrIter(const JsonArrIter&) = delete;
	JsonArrIter& operator=(const JsonArrIter&) = delete;

	bool IsMutable() const {
		return m_isMutable;
	}

	RefPtr<RefCountedMutDoc> m_pDocument_mut;
	RefPtr<RefCountedImmutableDoc> m_pDocument;

	yyjson_mut_arr_iter m_iterMut;
	yyjson_arr_iter m_iterImm;
	yyjson_mut_val* m_rootMut{ nullptr };
	yyjson_val* m_rootImm{ nullptr };

	SourceMod::Handle_t m_handle{ BAD_HANDLE };
	bool m_isMutable{ false };
	bool m_initialized{ false };
};

/**
 * @brief Object iterator wrapper
 *
 * Wraps yyjson_obj_iter and yyjson_mut_obj_iter for object iteration.
 */
class JsonObjIter {
public:
	JsonObjIter() = default;
	~JsonObjIter() = default;

	JsonObjIter(const JsonObjIter&) = delete;
	JsonObjIter& operator=(const JsonObjIter&) = delete;

	bool IsMutable() const {
		return m_isMutable;
	}

	RefPtr<RefCountedMutDoc> m_pDocument_mut;
	RefPtr<RefCountedImmutableDoc> m_pDocument;

	yyjson_mut_obj_iter m_iterMut;
	yyjson_obj_iter m_iterImm;
	yyjson_mut_val* m_rootMut{ nullptr };
	yyjson_val* m_rootImm{ nullptr };

	void* m_currentKey{ nullptr };

	SourceMod::Handle_t m_handle{ BAD_HANDLE };
	bool m_isMutable{ false };
	bool m_initialized{ false };
};

class JsonManager : public IJsonManager
{
public:
	JsonManager();
	~JsonManager();

public:
	// ========== Document Operations ==========
	virtual JsonValue* ParseJSON(const char* json_str, bool is_file, bool is_mutable,
		yyjson_read_flag read_flg, char* error, size_t error_size) override;
	virtual bool WriteToString(JsonValue* handle, char* buffer, size_t buffer_size,
		uint32_t write_flg, size_t* out_size, char* error, size_t error_size) override;
	virtual char* WriteToStringPtr(JsonValue* handle, yyjson_write_flag write_flg, size_t* out_size) override;
	virtual void ReleaseString(char* buffer) override;
	virtual JsonValue* ApplyJsonPatch(JsonValue* target, JsonValue* patch, bool result_mutable,
		char* error, size_t error_size) override;
	virtual bool JsonPatchInPlace(JsonValue* target, JsonValue* patch,
		char* error, size_t error_size) override;
	virtual JsonValue* ApplyMergePatch(JsonValue* target, JsonValue* patch, bool result_mutable,
		char* error, size_t error_size) override;
	virtual bool MergePatchInPlace(JsonValue* target, JsonValue* patch,
		char* error, size_t error_size) override;
	virtual bool WriteToFile(JsonValue* handle, const char* path, yyjson_write_flag write_flg,
		char* error, size_t error_size) override;
	virtual bool Equals(JsonValue* handle1, JsonValue* handle2) override;
	virtual bool EqualsStr(JsonValue* handle, const char* str) override;
	virtual JsonValue* DeepCopy(JsonValue* targetDoc, JsonValue* sourceValue,
		char* error = nullptr, size_t error_size = 0) override;
	virtual const char* GetTypeDesc(JsonValue* handle) override;
	virtual size_t GetSerializedSize(JsonValue* handle, yyjson_write_flag write_flg) override;
	virtual JsonValue* ToMutable(JsonValue* handle,
		char* error = nullptr, size_t error_size = 0) override;
	virtual JsonValue* ToImmutable(JsonValue* handle,
		char* error = nullptr, size_t error_size = 0) override;
	virtual yyjson_type GetType(JsonValue* handle) override;
	virtual yyjson_subtype GetSubtype(JsonValue* handle) override;
	virtual bool IsArray(JsonValue* handle) override;
	virtual bool IsObject(JsonValue* handle) override;
	virtual bool IsInt(JsonValue* handle) override;
	virtual bool IsUint(JsonValue* handle) override;
	virtual bool IsSint(JsonValue* handle) override;
	virtual bool IsNum(JsonValue* handle) override;
	virtual bool IsBool(JsonValue* handle) override;
	virtual bool IsTrue(JsonValue* handle) override;
	virtual bool IsFalse(JsonValue* handle) override;
	virtual bool IsFloat(JsonValue* handle) override;
	virtual bool IsStr(JsonValue* handle) override;
	virtual bool IsNull(JsonValue* handle) override;
	virtual bool IsCtn(JsonValue* handle) override;
	virtual bool IsMutable(JsonValue* handle) override;
	virtual bool IsImmutable(JsonValue* handle) override;
	virtual size_t GetReadSize(JsonValue* handle) override;
	virtual size_t GetRefCount(JsonValue* handle) override;
	virtual size_t GetValCount(JsonValue* handle) override;

	// ========== Object Operations ==========
	virtual JsonValue* ObjectInit() override;
	virtual JsonValue* ObjectInitWithStrings(const char** pairs, size_t count) override;
	virtual JsonValue* ObjectParseString(const char* str, yyjson_read_flag read_flg,
		char* error, size_t error_size) override;
	virtual JsonValue* ObjectParseFile(const char* path, yyjson_read_flag read_flg,
		char* error, size_t error_size) override;
	virtual size_t ObjectGetSize(JsonValue* handle) override;
	virtual bool ObjectGetKey(JsonValue* handle, size_t index, const char** out_key) override;
	virtual JsonValue* ObjectGetValueAt(JsonValue* handle, size_t index) override;
	virtual JsonValue* ObjectGet(JsonValue* handle, const char* key) override;
	virtual bool ObjectGetBool(JsonValue* handle, const char* key, bool* out_value) override;
	virtual bool ObjectGetDouble(JsonValue* handle, const char* key, double* out_value) override;
	virtual bool ObjectGetInt(JsonValue* handle, const char* key, int* out_value) override;
	virtual bool ObjectGetInt64(JsonValue* handle, const char* key, int64_t* out_value) override;
	virtual bool ObjectGetUint64(JsonValue* handle, const char* key, uint64_t* out_value) override;
	virtual bool ObjectGetString(JsonValue* handle, const char* key, const char** out_str, size_t* out_len) override;
	virtual bool ObjectIsNull(JsonValue* handle, const char* key, bool* out_is_null) override;
	virtual bool ObjectHasKey(JsonValue* handle, const char* key, bool use_pointer) override;
	virtual bool ObjectRenameKey(JsonValue* handle, const char* old_key, const char* new_key, bool allow_duplicate) override;
	virtual bool ObjectSet(JsonValue* handle, const char* key, JsonValue* value) override;
	virtual bool ObjectSetBool(JsonValue* handle, const char* key, bool value) override;
	virtual bool ObjectSetDouble(JsonValue* handle, const char* key, double value) override;
	virtual bool ObjectSetInt(JsonValue* handle, const char* key, int value) override;
	virtual bool ObjectSetInt64(JsonValue* handle, const char* key, int64_t value) override;
	virtual bool ObjectSetUint64(JsonValue* handle, const char* key, uint64_t value) override;
	virtual bool ObjectSetNull(JsonValue* handle, const char* key) override;
	virtual bool ObjectSetString(JsonValue* handle, const char* key, const char* value) override;
	virtual bool ObjectRemove(JsonValue* handle, const char* key) override;
	virtual bool ObjectClear(JsonValue* handle) override;
	virtual bool ObjectSort(JsonValue* handle, JSON_SORT_ORDER sort_mode) override;
	virtual bool ObjectRotate(JsonValue* handle, size_t idx) override;

	// ========== Array Operations ==========
	virtual JsonValue* ArrayInit() override;
	virtual JsonValue* ArrayInitWithStrings(const char** strings, size_t count) override;
	virtual JsonValue* ArrayInitWithInt32(const int32_t* values, size_t count,
		char* error = nullptr, size_t error_size = 0) override;
	virtual JsonValue* ArrayInitWithInt64(const char** values, size_t count,
		char* error, size_t error_size) override;
	virtual JsonValue* ArrayInitWithBool(const bool* values, size_t count,
		char* error = nullptr, size_t error_size = 0) override;
	virtual JsonValue* ArrayInitWithDouble(const double* values, size_t count,
		char* error = nullptr, size_t error_size = 0) override;
	virtual JsonValue* ArrayParseString(const char* str, yyjson_read_flag read_flg,
		char* error, size_t error_size) override;
	virtual JsonValue* ArrayParseFile(const char* path, yyjson_read_flag read_flg,
		char* error, size_t error_size) override;
	virtual size_t ArrayGetSize(JsonValue* handle) override;
	virtual JsonValue* ArrayGet(JsonValue* handle, size_t index) override;
	virtual JsonValue* ArrayGetFirst(JsonValue* handle) override;
	virtual JsonValue* ArrayGetLast(JsonValue* handle) override;
	virtual bool ArrayGetBool(JsonValue* handle, size_t index, bool* out_value) override;
	virtual bool ArrayGetDouble(JsonValue* handle, size_t index, double* out_value) override;
	virtual bool ArrayGetInt(JsonValue* handle, size_t index, int* out_value) override;
	virtual bool ArrayGetInt64(JsonValue* handle, size_t index, int64_t* out_value) override;
	virtual bool ArrayGetUint64(JsonValue* handle, size_t index, uint64_t* out_value) override;
	virtual bool ArrayGetString(JsonValue* handle, size_t index, const char** out_str, size_t* out_len) override;
	virtual bool ArrayIsNull(JsonValue* handle, size_t index) override;
	virtual bool ArrayReplace(JsonValue* handle, size_t index, JsonValue* value) override;
	virtual bool ArrayReplaceBool(JsonValue* handle, size_t index, bool value) override;
	virtual bool ArrayReplaceDouble(JsonValue* handle, size_t index, double value) override;
	virtual bool ArrayReplaceInt(JsonValue* handle, size_t index, int value) override;
	virtual bool ArrayReplaceInt64(JsonValue* handle, size_t index, int64_t value) override;
	virtual bool ArrayReplaceUint64(JsonValue* handle, size_t index, uint64_t value) override;
	virtual bool ArrayReplaceNull(JsonValue* handle, size_t index) override;
	virtual bool ArrayReplaceString(JsonValue* handle, size_t index, const char* value) override;
	virtual bool ArrayAppend(JsonValue* handle, JsonValue* value) override;
	virtual bool ArrayAppendBool(JsonValue* handle, bool value) override;
	virtual bool ArrayAppendDouble(JsonValue* handle, double value) override;
	virtual bool ArrayAppendInt(JsonValue* handle, int value) override;
	virtual bool ArrayAppendInt64(JsonValue* handle, int64_t value) override;
	virtual bool ArrayAppendUint64(JsonValue* handle, uint64_t value) override;
	virtual bool ArrayAppendNull(JsonValue* handle) override;
	virtual bool ArrayAppendString(JsonValue* handle, const char* value) override;
	virtual bool ArrayInsert(JsonValue* handle, size_t index, JsonValue* value) override;
	virtual bool ArrayInsertBool(JsonValue* handle, size_t index, bool value) override;
	virtual bool ArrayInsertInt(JsonValue* handle, size_t index, int value) override;
	virtual bool ArrayInsertInt64(JsonValue* handle, size_t index, int64_t value) override;
	virtual bool ArrayInsertUint64(JsonValue* handle, size_t index, uint64_t value) override;
	virtual bool ArrayInsertDouble(JsonValue* handle, size_t index, double value) override;
	virtual bool ArrayInsertString(JsonValue* handle, size_t index, const char* value) override;
	virtual bool ArrayInsertNull(JsonValue* handle, size_t index) override;
	virtual bool ArrayPrepend(JsonValue* handle, JsonValue* value) override;
	virtual bool ArrayPrependBool(JsonValue* handle, bool value) override;
	virtual bool ArrayPrependInt(JsonValue* handle, int value) override;
	virtual bool ArrayPrependInt64(JsonValue* handle, int64_t value) override;
	virtual bool ArrayPrependUint64(JsonValue* handle, uint64_t value) override;
	virtual bool ArrayPrependDouble(JsonValue* handle, double value) override;
	virtual bool ArrayPrependString(JsonValue* handle, const char* value) override;
	virtual bool ArrayPrependNull(JsonValue* handle) override;
	virtual bool ArrayRemove(JsonValue* handle, size_t index) override;
	virtual bool ArrayRemoveFirst(JsonValue* handle) override;
	virtual bool ArrayRemoveLast(JsonValue* handle) override;
	virtual bool ArrayRemoveRange(JsonValue* handle, size_t start_index, size_t count) override;
	virtual bool ArrayClear(JsonValue* handle) override;
	virtual int ArrayIndexOfBool(JsonValue* handle, bool search_value) override;
	virtual int ArrayIndexOfString(JsonValue* handle, const char* search_value) override;
	virtual int ArrayIndexOfInt(JsonValue* handle, int search_value) override;
	virtual int ArrayIndexOfInt64(JsonValue* handle, int64_t search_value) override;
	virtual int ArrayIndexOfUint64(JsonValue* handle, uint64_t search_value) override;
	virtual int ArrayIndexOfDouble(JsonValue* handle, double search_value) override;
	virtual bool ArraySort(JsonValue* handle, JSON_SORT_ORDER sort_mode) override;
	virtual bool ArrayRotate(JsonValue* handle, size_t idx) override;

	// ========== Value Operations ==========
	virtual JsonValue* Pack(const char* format, IPackParamProvider* param_provider, char* error, size_t error_size) override;
	virtual JsonValue* CreateBool(bool value) override;
	virtual JsonValue* CreateDouble(double value) override;
	virtual JsonValue* CreateInt(int value) override;
	virtual JsonValue* CreateInt64(int64_t value,
		char* error = nullptr, size_t error_size = 0) override;
	virtual JsonValue* CreateUint64(uint64_t value,
		char* error = nullptr, size_t error_size = 0) override;
	virtual JsonValue* CreateNull() override;
	virtual JsonValue* CreateString(const char* value) override;
	virtual bool GetBool(JsonValue* handle, bool* out_value) override;
	virtual bool GetDouble(JsonValue* handle, double* out_value) override;
	virtual bool GetInt(JsonValue* handle, int* out_value) override;
	virtual bool GetInt64(JsonValue* handle, int64_t* out_value) override;
	virtual bool GetUint64(JsonValue* handle, uint64_t* out_value) override;
	virtual bool GetString(JsonValue* handle, const char** out_str, size_t* out_len) override;

	// ========== Pointer Operations ==========
	virtual JsonValue* PtrGet(JsonValue* handle, const char* path, char* error, size_t error_size) override;
	virtual bool PtrGetBool(JsonValue* handle, const char* path, bool* out_value, char* error, size_t error_size) override;
	virtual bool PtrGetDouble(JsonValue* handle, const char* path, double* out_value, char* error, size_t error_size) override;
	virtual bool PtrGetInt(JsonValue* handle, const char* path, int* out_value, char* error, size_t error_size) override;
	virtual bool PtrGetInt64(JsonValue* handle, const char* path, int64_t* out_value, char* error, size_t error_size) override;
	virtual bool PtrGetUint64(JsonValue* handle, const char* path, uint64_t* out_value, char* error, size_t error_size) override;
	virtual bool PtrGetString(JsonValue* handle, const char* path, const char** out_str, size_t* out_len, char* error, size_t error_size) override;
	virtual bool PtrGetIsNull(JsonValue* handle, const char* path, bool* out_is_null, char* error, size_t error_size) override;
	virtual bool PtrGetLength(JsonValue* handle, const char* path, size_t* out_len, char* error, size_t error_size) override;
	virtual bool PtrSet(JsonValue* handle, const char* path, JsonValue* value, char* error, size_t error_size) override;
	virtual bool PtrSetBool(JsonValue* handle, const char* path, bool value, char* error, size_t error_size) override;
	virtual bool PtrSetDouble(JsonValue* handle, const char* path, double value, char* error, size_t error_size) override;
	virtual bool PtrSetInt(JsonValue* handle, const char* path, int value, char* error, size_t error_size) override;
	virtual bool PtrSetInt64(JsonValue* handle, const char* path, int64_t value, char* error, size_t error_size) override;
	virtual bool PtrSetUint64(JsonValue* handle, const char* path, uint64_t value, char* error, size_t error_size) override;
	virtual bool PtrSetString(JsonValue* handle, const char* path, const char* value, char* error, size_t error_size) override;
	virtual bool PtrSetNull(JsonValue* handle, const char* path, char* error, size_t error_size) override;
	virtual bool PtrAdd(JsonValue* handle, const char* path, JsonValue* value, char* error, size_t error_size) override;
	virtual bool PtrAddBool(JsonValue* handle, const char* path, bool value, char* error, size_t error_size) override;
	virtual bool PtrAddDouble(JsonValue* handle, const char* path, double value, char* error, size_t error_size) override;
	virtual bool PtrAddInt(JsonValue* handle, const char* path, int value, char* error, size_t error_size) override;
	virtual bool PtrAddInt64(JsonValue* handle, const char* path, int64_t value, char* error, size_t error_size) override;
	virtual bool PtrAddUint64(JsonValue* handle, const char* path, uint64_t value, char* error, size_t error_size) override;
	virtual bool PtrAddString(JsonValue* handle, const char* path, const char* value, char* error, size_t error_size) override;
	virtual bool PtrAddNull(JsonValue* handle, const char* path, char* error, size_t error_size) override;
	virtual bool PtrRemove(JsonValue* handle, const char* path, char* error, size_t error_size) override;
	virtual JsonValue* PtrTryGet(JsonValue* handle, const char* path) override;
	virtual bool PtrTryGetBool(JsonValue* handle, const char* path, bool* out_value) override;
	virtual bool PtrTryGetDouble(JsonValue* handle, const char* path, double* out_value) override;
	virtual bool PtrTryGetInt(JsonValue* handle, const char* path, int* out_value) override;
	virtual bool PtrTryGetInt64(JsonValue* handle, const char* path, int64_t* out_value) override;
	virtual bool PtrTryGetUint64(JsonValue* handle, const char* path, uint64_t* out_value) override;
	virtual bool PtrTryGetString(JsonValue* handle, const char* path, const char** out_str, size_t* out_len) override;

	// ========== Array Iterator Operations ==========
	virtual JsonArrIter* ArrIterInit(JsonValue* handle) override;
	virtual JsonArrIter* ArrIterWith(JsonValue* handle) override;
	virtual bool ArrIterReset(JsonArrIter* iter) override;
	virtual JsonValue* ArrIterNext(JsonArrIter* iter) override;
	virtual bool ArrIterHasNext(JsonArrIter* iter) override;
	virtual size_t ArrIterGetIndex(JsonArrIter* iter) override;
	virtual bool ArrIterRemove(JsonArrIter* iter) override;

	// ========== Object Iterator Operations ==========
	virtual JsonObjIter* ObjIterInit(JsonValue* handle) override;
	virtual JsonObjIter* ObjIterWith(JsonValue* handle) override;
	virtual bool ObjIterReset(JsonObjIter* iter) override;
	virtual bool ObjIterNext(JsonObjIter* iter, const char** out_key, size_t* out_len = nullptr) override;
	virtual bool ObjIterHasNext(JsonObjIter* iter) override;
	virtual JsonValue* ObjIterGetVal(JsonObjIter* iter) override;
	virtual JsonValue* ObjIterGet(JsonObjIter* iter, const char* key) override;
	virtual size_t ObjIterGetIndex(JsonObjIter* iter) override;
	virtual bool ObjIterRemove(JsonObjIter* iter) override;

	// ========== Iterator Release Operations ==========
	virtual void ReleaseArrIter(JsonArrIter* iter) override;
	virtual void ReleaseObjIter(JsonObjIter* iter) override;

	// ========== Iterator Handle Type Operations ==========
	virtual SourceMod::HandleType_t GetArrIterHandleType() override;
	virtual SourceMod::HandleType_t GetObjIterHandleType() override;
	virtual JsonArrIter* GetArrIterFromHandle(SourcePawn::IPluginContext* pContext, SourceMod::Handle_t handle) override;
	virtual JsonObjIter* GetObjIterFromHandle(SourcePawn::IPluginContext* pContext, SourceMod::Handle_t handle) override;

	// ========== Release Operations ==========
	virtual void ReleaseJsonValue(JsonValue* value) override;

	// ========== Handle Type Operations ==========
	virtual SourceMod::HandleType_t GetJsonHandleType() override;

	// ========== Handle Operations ==========
	virtual JsonValue* GetValueFromHandle(SourcePawn::IPluginContext* pContext, SourceMod::Handle_t handle) override;

	// ========== Number Read/Write Operations ==========
	virtual JsonValue* ReadNumber(const char* dat, uint32_t read_flg = 0,
		char* error = nullptr, size_t error_size = 0, size_t* out_consumed = nullptr) override;
	virtual bool WriteNumber(JsonValue* handle, char* buffer, size_t buffer_size,
		size_t* out_written = nullptr) override;

	// ========== Floating-Point Format Operations ==========
	virtual bool SetFpToFloat(JsonValue* handle, bool flt) override;
	virtual bool SetFpToFixed(JsonValue* handle, int prec) override;

	// ========== Direct Value Modification Operations ==========
	virtual bool SetBool(JsonValue* handle, bool value) override;
	virtual bool SetInt(JsonValue* handle, int value) override;
	virtual bool SetInt64(JsonValue* handle, int64_t value) override;
	virtual bool SetUint64(JsonValue* handle, uint64_t value) override;
	virtual bool SetDouble(JsonValue* handle, double value) override;
	virtual bool SetString(JsonValue* handle, const char* value) override;
	virtual bool SetNull(JsonValue* handle) override;


private:
	std::random_device m_randomDevice;
	std::mt19937 m_randomGenerator;

	// Helper methods
	static std::unique_ptr<JsonValue> CreateWrapper();
	static RefPtr<RefCountedMutDoc> WrapDocument(yyjson_mut_doc* doc);
	static RefPtr<RefCountedMutDoc> CopyDocument(yyjson_doc* doc);
	static RefPtr<RefCountedMutDoc> CreateDocument();
	static RefPtr<RefCountedImmutableDoc> WrapImmutableDocument(yyjson_doc* doc);
	static RefPtr<RefCountedMutDoc> CloneValueToMutable(JsonValue* value);

	// Pointer operation helpers
	enum class PtrMutationOp {
		Set,
		Add
	};

	struct PtrResolvedValue {
		yyjson_mut_val* mut{ nullptr };
		yyjson_val* imm{ nullptr };
		bool is_mutable{ false };
	};

	static void SetPtrOperationError(const char* action, const yyjson_ptr_err& ptr_error,
		const char* path, char* error, size_t error_size);
	static bool ApplyPtrMutation(JsonValue* handle, const char* path,
		yyjson_mut_val* val, PtrMutationOp op,
		char* error, size_t error_size, const char* value_error);
	static bool ValidateMutablePtrParams(JsonValue* handle,
		const char* path, char* error, size_t error_size);
	static bool ResolvePtrValue(JsonValue* handle, const char* path,
		PtrResolvedValue* out, char* error, size_t error_size);
	static bool ReportPtrTypeMismatch(const PtrResolvedValue& resolved,
		const char* path, const char* expected, char* error, size_t error_size);

	enum class ContainerRootType {
		Object,
		Array
	};

	static yyjson_doc* ReadJsonDocument(const char* input, bool is_file,
		yyjson_read_flag read_flg, const char* parse_string_error_fmt,
		bool use_resolved_path_in_error, char* error, size_t error_size);
	static JsonValue* ParseTypedRootValue(const char* input, bool is_file,
		yyjson_read_flag read_flg, ContainerRootType expected_type,
		const char* invalid_input_error,
		const char* parse_string_error_fmt,
		const char* root_string_error_fmt,
		const char* root_file_error_fmt,
		char* error, size_t error_size);

	enum class PatchOperation {
		JsonPatch,
		MergePatch
	};

	static bool PreparePatchExecution(yyjson_mut_doc* doc, JsonValue* patch,
		yyjson_mut_val** out_root, yyjson_mut_val** out_patch_copy,
		char* error, size_t error_size);
	static yyjson_mut_val* ExecutePatchOperation(PatchOperation op,
		yyjson_mut_doc* doc, yyjson_mut_val* root, yyjson_mut_val* patch_copy,
		bool in_place, char* error, size_t error_size);
	static JsonValue* WrapPatchedDocument(RefPtr<RefCountedMutDoc> doc_ref,
		bool result_mutable, char* error, size_t error_size);

	// PtrTryGet helper methods
	struct PtrGetValueResult {
		yyjson_mut_val* mut_val{ nullptr };
		yyjson_val* imm_val{ nullptr };
		bool success{ false };
	};
	static PtrGetValueResult PtrGetValueInternal(JsonValue* handle, const char* path);
};
