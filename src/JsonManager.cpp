#include "JsonManager.h"
#include "JsonTemplates.h"
#include "extension.h"
#include <cctype>
#include <cmath>
#include <variant>

static bool ParseInt64Variant(const char* value, std::variant<int64_t, uint64_t>* out_value,
	char* error, size_t error_size);

static inline bool ReadInt64FromVal(yyjson_val* val, int64_t* out_value) {
	if (!yyjson_is_sint(val)) {
		return false;
	}
	*out_value = yyjson_get_sint(val);
	return true;
}

static inline bool ReadUint64FromVal(yyjson_val* val, uint64_t* out_value) {
	if (!yyjson_is_uint(val)) {
		return false;
	}
	*out_value = yyjson_get_uint(val);
	return true;
}

static inline bool ReadInt64FromMutVal(yyjson_mut_val* val, int64_t* out_value) {
	if (!yyjson_mut_is_sint(val)) {
		return false;
	}
	*out_value = yyjson_mut_get_sint(val);
	return true;
}

static inline bool ReadUint64FromMutVal(yyjson_mut_val* val, uint64_t* out_value) {
	if (!yyjson_mut_is_uint(val)) {
		return false;
	}
	*out_value = yyjson_mut_get_uint(val);
	return true;
}

static bool EqualsFloatingPoint(double a, double b)
{
	if (std::isnan(a) || std::isnan(b)) {
		return false;
	}
	if (a == b) {
		return true;
	}
	if (std::isinf(a) || std::isinf(b)) {
		return std::isinf(a) && std::isinf(b) && (a > 0) == (b > 0);
	}

	double diff = std::fabs(a - b);
	return diff <= 1e-15 || diff <= std::fmax(std::fabs(a), std::fabs(b)) * 1e-6;
}

std::unique_ptr<JsonValue> JsonManager::CreateWrapper() {
	return std::make_unique<JsonValue>();
}

RefPtr<RefCountedMutDoc> JsonManager::WrapDocument(yyjson_mut_doc* doc) {
	if (!doc) {
		return RefPtr<RefCountedMutDoc>();
	}
	return make_ref<RefCountedMutDoc>(doc);
}

RefPtr<RefCountedMutDoc> JsonManager::CopyDocument(yyjson_doc* doc) {
	return WrapDocument(yyjson_doc_mut_copy(doc, nullptr));
}

RefPtr<RefCountedMutDoc> JsonManager::CreateDocument() {
	return WrapDocument(yyjson_mut_doc_new(nullptr));
}

RefPtr<RefCountedImmutableDoc> JsonManager::WrapImmutableDocument(yyjson_doc* doc) {
	if (!doc) {
		return RefPtr<RefCountedImmutableDoc>();
	}
	return make_ref<RefCountedImmutableDoc>(doc);
}

RefPtr<RefCountedMutDoc> JsonManager::CloneValueToMutable(JsonValue* value) {
	if (!value) {
		return RefPtr<RefCountedMutDoc>();
	}

	if (value->IsMutable()) {
		yyjson_mut_doc* dup = yyjson_mut_doc_mut_copy(value->m_pDocument_mut->get(), nullptr);
		return WrapDocument(dup);
	}

	if (!value->m_pDocument) {
		return RefPtr<RefCountedMutDoc>();
	}

	return CopyDocument(value->m_pDocument->get());
}

static yyjson_mut_val* CopyValueIntoDoc(JsonValue* value, yyjson_mut_doc* doc, char* error, size_t error_size) {
	NativeErrorBuffer::Clear(error, error_size);

	if (!value || !doc) {
		NativeErrorBuffer::Set(error, error_size, "Invalid JSON value or document");
		return nullptr;
	}

	yyjson_mut_val* copy = nullptr;
	if (value->IsMutable()) {
		if (!value->m_pVal_mut) {
			NativeErrorBuffer::Set(error, error_size, "Mutable JSON value has no root");
			return nullptr;
		}
		copy = yyjson_mut_val_mut_copy(doc, value->m_pVal_mut);
	} else {
		if (!value->m_pVal) {
			NativeErrorBuffer::Set(error, error_size, "Immutable JSON value has no root");
			return nullptr;
		}
		copy = yyjson_val_mut_copy(doc, value->m_pVal);
	}

	if (!copy) {
		NativeErrorBuffer::Set(error, error_size, "Failed to copy JSON value");
	}
	return copy;
}

void JsonManager::SetPtrOperationError(const char* action,
	const yyjson_ptr_err& ptr_error, const char* path,
	char* error, size_t error_size)
{
	const char* msg = ptr_error.msg ? ptr_error.msg : "unknown error";
	NativeErrorBuffer::Set(error, error_size,
		"Failed to %s JSON pointer: %s (error code: %u, position: %zu, path: %s)",
		action, msg, ptr_error.code, ptr_error.pos, path);
}

bool JsonManager::ApplyPtrMutation(JsonValue* handle, const char* path,
	yyjson_mut_val* val, PtrMutationOp op,
	char* error, size_t error_size, const char* value_error)
{
	if (!val) {
		NativeErrorBuffer::Set(error, error_size, "%s", value_error);
		return false;
	}

	size_t path_len = strlen(path);
	yyjson_ptr_err ptr_error{};
	bool success = false;

	if (op == PtrMutationOp::Set) {
		success = yyjson_mut_doc_ptr_setx(handle->m_pDocument_mut->get(), path, path_len,
			val, true, nullptr, &ptr_error);
	} else {
		success = yyjson_mut_doc_ptr_addx(handle->m_pDocument_mut->get(), path, path_len,
			val, true, nullptr, &ptr_error);
	}

	if (!success && ptr_error.code) {
		const char* action = (op == PtrMutationOp::Set) ? "set" : "add";
		SetPtrOperationError(action, ptr_error, path, error, error_size);
	}

	return success;
}

bool JsonManager::ValidateMutablePtrParams(JsonValue* handle, const char* path,
	char* error, size_t error_size)
{
	if (!handle || !handle->IsMutable() || !path) {
		NativeErrorBuffer::Set(error, error_size, "Invalid parameters or immutable document");
		return false;
	}
	return true;
}

bool JsonManager::ResolvePtrValue(JsonValue* handle, const char* path,
	PtrResolvedValue* out, char* error, size_t error_size)
{
	if (!handle || !path || !out) {
		NativeErrorBuffer::Set(error, error_size, "Invalid parameters");
		return false;
	}

	if (handle->IsMutable()) {
		if (!handle->m_pDocument_mut) {
			NativeErrorBuffer::Set(error, error_size, "Invalid parameters or immutable document");
			return false;
		}
	} else {
		if (!handle->m_pDocument) {
			NativeErrorBuffer::Set(error, error_size, "Invalid parameters");
			return false;
		}
	}

	size_t path_len = strlen(path);
	yyjson_ptr_err ptr_error{};

	if (handle->IsMutable()) {
		out->is_mutable = true;
		out->mut = yyjson_mut_doc_ptr_getx(handle->m_pDocument_mut->get(), path,
			path_len, nullptr, &ptr_error);
		if (!out->mut || ptr_error.code) {
			SetPtrOperationError("resolve", ptr_error, path, error, error_size);
			return false;
		}
	} else {
		out->is_mutable = false;
		out->imm = yyjson_doc_ptr_getx(handle->m_pDocument->get(), path,
			path_len, &ptr_error);
		if (!out->imm || ptr_error.code) {
			SetPtrOperationError("resolve", ptr_error, path, error, error_size);
			return false;
		}
	}

	return true;
}

bool JsonManager::ReportPtrTypeMismatch(const PtrResolvedValue& resolved,
	const char* path, const char* expected, char* error, size_t error_size)
{
	const char* actual = resolved.is_mutable
		? yyjson_mut_get_type_desc(resolved.mut)
		: yyjson_get_type_desc(resolved.imm);
	NativeErrorBuffer::Set(error, error_size,
		"Type mismatch at path '%s': expected %s, got %s", path, expected, actual);
	return false;
}

yyjson_doc* JsonManager::ReadJsonDocument(const char* input, bool is_file,
	yyjson_read_flag read_flg, const char* parse_string_error_fmt,
	bool use_resolved_path_in_error, char* error, size_t error_size)
{
	if (!input) {
		return nullptr;
	}

	char realpath[PLATFORM_MAX_PATH];
	const char* error_target = input;
	yyjson_read_err read_error{};
	yyjson_doc* idoc = nullptr;

	if (is_file) {
		smutils->BuildPath(Path_Game, realpath, sizeof(realpath), "%s", input);
		error_target = use_resolved_path_in_error ? realpath : input;
		idoc = yyjson_read_file(realpath, read_flg, nullptr, &read_error);
	} else {
		idoc = yyjson_read_opts(const_cast<char*>(input), strlen(input), read_flg, nullptr, &read_error);
	}

	if (!idoc || read_error.code) {
		const char* msg = read_error.msg ? read_error.msg : "unknown error";
		if (is_file) {
			NativeErrorBuffer::Set(error, error_size,
				"Failed to parse JSON file: %s (error code: %u, msg: %s, position: %zu)",
				error_target, read_error.code, msg, read_error.pos);
		} else {
			const char* parse_error_fmt = parse_string_error_fmt
				? parse_string_error_fmt
				: "Failed to parse JSON str: %s (error code: %u, position: %zu)";
			NativeErrorBuffer::Set(error, error_size, parse_error_fmt,
				msg, read_error.code, read_error.pos);
		}
		return nullptr;
	}

	return idoc;
}

JsonValue* JsonManager::ParseTypedRootValue(const char* input, bool is_file,
	yyjson_read_flag read_flg, ContainerRootType expected_type,
	const char* invalid_input_error,
	const char* parse_string_error_fmt,
	const char* root_string_error_fmt,
	const char* root_file_error_fmt,
	char* error, size_t error_size)
{
	NativeErrorBuffer::Clear(error, error_size);

	if (!input) {
		NativeErrorBuffer::Set(error, error_size, "%s", invalid_input_error);
		return nullptr;
	}

	yyjson_doc* idoc = ReadJsonDocument(input, is_file, read_flg,
		parse_string_error_fmt, true, error, error_size);
	if (!idoc) {
		return nullptr;
	}

	yyjson_val* root = yyjson_doc_get_root(idoc);
	bool root_ok = (expected_type == ContainerRootType::Object)
		? yyjson_is_obj(root)
		: yyjson_is_arr(root);

	if (!root_ok) {
		const char* type_desc = yyjson_get_type_desc(root);
		if (is_file) {
			NativeErrorBuffer::Set(error, error_size, root_file_error_fmt, type_desc);
		} else {
			NativeErrorBuffer::Set(error, error_size, root_string_error_fmt, type_desc);
		}
		yyjson_doc_free(idoc);
		return nullptr;
	}

	auto value = CreateWrapper();
	value->m_readSize = yyjson_doc_get_read_size(idoc);
	value->m_pDocument = WrapImmutableDocument(idoc);
	value->m_pVal = root;
	return value.release();
}

bool JsonManager::PreparePatchExecution(yyjson_mut_doc* doc, JsonValue* patch,
	yyjson_mut_val** out_root, yyjson_mut_val** out_patch_copy,
	char* error, size_t error_size)
{
	if (!patch || !out_root || !out_patch_copy) {
		NativeErrorBuffer::Set(error, error_size, "Invalid parameters");
		return false;
	}

	if (!doc) {
		NativeErrorBuffer::Set(error, error_size, "Target JSON has no root value");
		return false;
	}

	yyjson_mut_val* root = yyjson_mut_doc_get_root(doc);
	if (!root) {
		NativeErrorBuffer::Set(error, error_size, "Target JSON has no root value");
		return false;
	}

	yyjson_mut_val* patch_copy = CopyValueIntoDoc(patch, doc, error, error_size);
	if (!patch_copy) {
		return false;
	}

	*out_root = root;
	*out_patch_copy = patch_copy;
	return true;
}

yyjson_mut_val* JsonManager::ExecutePatchOperation(PatchOperation op,
	yyjson_mut_doc* doc, yyjson_mut_val* root, yyjson_mut_val* patch_copy,
	bool in_place, char* error, size_t error_size)
{
	if (op == PatchOperation::JsonPatch) {
		yyjson_patch_err patch_err{};
		yyjson_mut_val* result_root = yyjson_mut_patch(doc, root, patch_copy, &patch_err);
		if (!result_root) {
			const char* msg = patch_err.msg ? patch_err.msg : "unknown error";
			NativeErrorBuffer::Set(error, error_size,
				"JSON patch failed (code %u, op index %zu, message: %s)",
				patch_err.code, patch_err.idx, msg);
		}
		return result_root;
	}

	yyjson_mut_val* result_root = yyjson_mut_merge_patch(doc, root, patch_copy);
	if (!result_root) {
		if (in_place) {
			NativeErrorBuffer::Set(error, error_size, "Failed to apply JSON Merge Patch in place");
		} else {
			NativeErrorBuffer::Set(error, error_size, "Failed to apply JSON Merge Patch");
		}
	}
	return result_root;
}

JsonValue* JsonManager::WrapPatchedDocument(RefPtr<RefCountedMutDoc> doc_ref,
	bool result_mutable, char* error, size_t error_size)
{
	if (!doc_ref) {
		NativeErrorBuffer::Set(error, error_size, "Failed to clone target JSON value");
		return nullptr;
	}

	yyjson_mut_doc* doc = doc_ref->get();
	if (result_mutable) {
		auto wrapper = CreateWrapper();
		wrapper->m_pDocument_mut = doc_ref;
		wrapper->m_pVal_mut = yyjson_mut_doc_get_root(doc);
		return wrapper.release();
	}

	yyjson_doc* imut_doc = yyjson_mut_doc_imut_copy(doc, nullptr);
	if (!imut_doc) {
		NativeErrorBuffer::Set(error, error_size, "Failed to convert patched JSON to immutable document");
		return nullptr;
	}

	auto wrapper = CreateWrapper();
	wrapper->m_pDocument = WrapImmutableDocument(imut_doc);
	if (!wrapper->m_pDocument) {
		yyjson_doc_free(imut_doc);
		NativeErrorBuffer::Set(error, error_size, "Failed to wrap immutable JSON document");
		return nullptr;
	}

	wrapper->m_pVal = yyjson_doc_get_root(imut_doc);
	return wrapper.release();
}

JsonManager::JsonManager(): m_randomGenerator(m_randomDevice()) {}

JsonManager::~JsonManager() {}

JsonValue* JsonManager::ParseJSON(const char* json_str, bool is_file, bool is_mutable,
	yyjson_read_flag read_flg, char* error, size_t error_size)
{
	NativeErrorBuffer::Clear(error, error_size);

	if (!json_str) {
		NativeErrorBuffer::Set(error, error_size, "Invalid JSON string");
		return nullptr;
	}

	auto pJSONValue = CreateWrapper();
	yyjson_doc* idoc = ReadJsonDocument(json_str, is_file, read_flg,
		"Failed to parse JSON str: %s (error code: %u, position: %zu)",
		false, error, error_size);
	if (!idoc) {
		return nullptr;
	}

	pJSONValue->m_readSize = yyjson_doc_get_read_size(idoc);

	if (is_mutable) {
		pJSONValue->m_pDocument_mut = CopyDocument(idoc);
		yyjson_doc_free(idoc);
		if (!pJSONValue->m_pDocument_mut) {
			NativeErrorBuffer::Set(error, error_size, "Failed to create mutable JSON document");
			return nullptr;
		}
		pJSONValue->m_pVal_mut = yyjson_mut_doc_get_root(pJSONValue->m_pDocument_mut->get());
		if (!pJSONValue->m_pVal_mut) {
			NativeErrorBuffer::Set(error, error_size, "Mutable JSON document has no root value");
			return nullptr;
		}
	} else {
		pJSONValue->m_pDocument = WrapImmutableDocument(idoc);
		if (!pJSONValue->m_pDocument) {
			yyjson_doc_free(idoc);
			NativeErrorBuffer::Set(error, error_size, "Failed to create immutable JSON document");
			return nullptr;
		}
		pJSONValue->m_pVal = yyjson_doc_get_root(pJSONValue->m_pDocument->get());
		if (!pJSONValue->m_pVal) {
			NativeErrorBuffer::Set(error, error_size, "Immutable JSON document has no root value");
			return nullptr;
		}
	}

	return pJSONValue.release();
}

bool JsonManager::WriteToString(JsonValue* handle, char* buffer, size_t buffer_size,
	uint32_t write_flg, size_t* out_size, char* error, size_t error_size)
{
	NativeErrorBuffer::Clear(error, error_size);

	if (!handle || !buffer || buffer_size == 0) {
		NativeErrorBuffer::Set(error, error_size, "Invalid parameters");
		return false;
	}

	if (buffer_size == 1) {
		NativeErrorBuffer::Set(error, error_size, "Buffer is too small");
		return false;
	}

	size_t usable_size = buffer_size - 1; // reserve space for null terminator
	size_t written;
	yyjson_write_err writeError{};

	if (handle->IsMutable()) {
		written = yyjson_mut_val_write_buf(buffer, usable_size, handle->m_pVal_mut, write_flg, &writeError);
	} else {
		written = yyjson_val_write_buf(buffer, usable_size, handle->m_pVal, write_flg, &writeError);
	}

	if (writeError.code) {
		const char* msg = writeError.msg ? writeError.msg : "unknown error";
		NativeErrorBuffer::Set(error, error_size, "Failed to serialize JSON: %s (error code: %u)", msg, writeError.code);
		return false;
	}

	// Ensure space is available for terminator (written <= usable_size by contract)
	buffer[written] = '\0';

	if (out_size) {
		*out_size = written + 1;
	}
	return true;
}

char* JsonManager::WriteToStringPtr(JsonValue* handle, yyjson_write_flag write_flg, size_t* out_size)
{
	if (!handle) {
		if (out_size) *out_size = 0;
		return nullptr;
	}

	size_t json_size = 0;
	char* json_str;

	if (handle->IsMutable()) {
		json_str = yyjson_mut_val_write(handle->m_pVal_mut, write_flg, &json_size);
	} else {
		json_str = yyjson_val_write(handle->m_pVal, write_flg, &json_size);
	}

	if (out_size) {
		*out_size = json_str ? (json_size + 1) : 0;
	}

	return json_str;
}

void JsonManager::ReleaseString(char* buffer)
{
	free(buffer);
}

JsonValue* JsonManager::ApplyJsonPatch(JsonValue* target, JsonValue* patch, bool result_mutable,
	char* error, size_t error_size)
{
	NativeErrorBuffer::Clear(error, error_size);

	if (!target || !patch) {
		NativeErrorBuffer::Set(error, error_size, "Target or patch JSON value is null");
		return nullptr;
	}

	auto doc_ref = CloneValueToMutable(target);
	if (!doc_ref) {
		NativeErrorBuffer::Set(error, error_size, "Failed to clone target JSON value");
		return nullptr;
	}

	yyjson_mut_doc* doc = doc_ref->get();
	yyjson_mut_val* root = nullptr;
	yyjson_mut_val* patch_copy = nullptr;
	if (!PreparePatchExecution(doc, patch, &root, &patch_copy, error, error_size)) {
		return nullptr;
	}

	yyjson_mut_val* result_root = ExecutePatchOperation(PatchOperation::JsonPatch,
		doc, root, patch_copy, false, error, error_size);
	if (!result_root) {
		return nullptr;
	}

	yyjson_mut_doc_set_root(doc, result_root);
	return WrapPatchedDocument(doc_ref, result_mutable, error, error_size);
}

bool JsonManager::JsonPatchInPlace(JsonValue* target, JsonValue* patch,
	char* error, size_t error_size)
{
	NativeErrorBuffer::Clear(error, error_size);

	if (!target || !patch) {
		NativeErrorBuffer::Set(error, error_size, "Target or patch JSON value is null");
		return false;
	}

	if (!target->IsMutable()) {
		NativeErrorBuffer::Set(error, error_size, "Target JSON must be mutable for in-place JSON Patch");
		return false;
	}

	yyjson_mut_doc* doc = target->m_pDocument_mut ? target->m_pDocument_mut->get() : nullptr;
	yyjson_mut_val* root = nullptr;
	yyjson_mut_val* patch_copy = nullptr;
	if (!PreparePatchExecution(doc, patch, &root, &patch_copy, error, error_size)) {
		return false;
	}

	yyjson_mut_val* result_root = ExecutePatchOperation(PatchOperation::JsonPatch,
		doc, root, patch_copy, true, error, error_size);
	if (!result_root) {
		return false;
	}

	yyjson_mut_doc_set_root(doc, result_root);
	target->m_pVal_mut = yyjson_mut_doc_get_root(doc);
	return true;
}

JsonValue* JsonManager::ApplyMergePatch(JsonValue* target, JsonValue* patch, bool result_mutable,
	char* error, size_t error_size)
{
	NativeErrorBuffer::Clear(error, error_size);

	if (!target || !patch) {
		NativeErrorBuffer::Set(error, error_size, "Target or patch JSON value is null");
		return nullptr;
	}

	auto doc_ref = CloneValueToMutable(target);
	if (!doc_ref) {
		NativeErrorBuffer::Set(error, error_size, "Failed to clone target JSON value");
		return nullptr;
	}

	yyjson_mut_doc* doc = doc_ref->get();
	yyjson_mut_val* root = nullptr;
	yyjson_mut_val* patch_copy = nullptr;
	if (!PreparePatchExecution(doc, patch, &root, &patch_copy, error, error_size)) {
		return nullptr;
	}

	yyjson_mut_val* result_root = ExecutePatchOperation(PatchOperation::MergePatch,
		doc, root, patch_copy, false, error, error_size);
	if (!result_root) {
		return nullptr;
	}

	yyjson_mut_doc_set_root(doc, result_root);
	return WrapPatchedDocument(doc_ref, result_mutable, error, error_size);
}

bool JsonManager::MergePatchInPlace(JsonValue* target, JsonValue* patch,
	char* error, size_t error_size)
{
	NativeErrorBuffer::Clear(error, error_size);

	if (!target || !patch) {
		NativeErrorBuffer::Set(error, error_size, "Target or patch JSON value is null");
		return false;
	}

	if (!target->IsMutable()) {
		NativeErrorBuffer::Set(error, error_size, "Target JSON must be mutable for in-place merge patch");
		return false;
	}

	yyjson_mut_doc* doc = target->m_pDocument_mut ? target->m_pDocument_mut->get() : nullptr;
	yyjson_mut_val* root = nullptr;
	yyjson_mut_val* patch_copy = nullptr;
	if (!PreparePatchExecution(doc, patch, &root, &patch_copy, error, error_size)) {
		return false;
	}

	yyjson_mut_val* result_root = ExecutePatchOperation(PatchOperation::MergePatch,
		doc, root, patch_copy, true, error, error_size);
	if (!result_root) {
		return false;
	}

	yyjson_mut_doc_set_root(doc, result_root);
	target->m_pVal_mut = yyjson_mut_doc_get_root(doc);
	return true;
}

bool JsonManager::WriteToFile(JsonValue* handle, const char* path, yyjson_write_flag write_flg,
	char* error, size_t error_size)
{
	NativeErrorBuffer::Clear(error, error_size);

	if (!handle || !path) {
		NativeErrorBuffer::Set(error, error_size, "Invalid parameters");
		return false;
	}

	char realpath[PLATFORM_MAX_PATH];
	smutils->BuildPath(Path_Game, realpath, sizeof(realpath), "%s", path);

	yyjson_write_err writeError{};
	bool is_success;

	if (handle->IsMutable()) {
		is_success = yyjson_mut_write_file(realpath, handle->m_pDocument_mut->get(), write_flg, nullptr, &writeError);
	} else {
		is_success = yyjson_write_file(realpath, handle->m_pDocument->get(), write_flg, nullptr, &writeError);
	}

	if (!is_success) {
		if (writeError.code) {
			const char* msg = writeError.msg ? writeError.msg : "unknown error";
			NativeErrorBuffer::Set(error, error_size,
				"Failed to write JSON to file: %s (error code: %u)", msg, writeError.code);
		} else {
			NativeErrorBuffer::Set(error, error_size, "Failed to write JSON to file (unknown error)");
		}
	}

	return is_success;
}

bool JsonManager::Equals(JsonValue* handle1, JsonValue* handle2)
{
	if (!handle1 || !handle2) {
		return false;
	}

	if (handle1->IsMutable() && handle2->IsMutable()) {
		return yyjson_mut_equals(handle1->m_pVal_mut, handle2->m_pVal_mut);
	}

	if (!handle1->IsMutable() && !handle2->IsMutable()) {
		return yyjson_equals(handle1->m_pVal, handle2->m_pVal);
	}

	JsonValue* immutable = handle1->IsMutable() ? handle2 : handle1;
	JsonValue* mutable_doc = handle1->IsMutable() ? handle1 : handle2;

	auto doc_mut = CopyDocument(immutable->m_pDocument->get());
	if (!doc_mut) {
		return false;
	}

	yyjson_mut_val* val_mut = yyjson_mut_doc_get_root(doc_mut->get());
	if (!val_mut) {
		return false;
	}

	return yyjson_mut_equals(mutable_doc->m_pVal_mut, val_mut);
}

bool JsonManager::EqualsStr(JsonValue* handle, const char* str)
{
	if (!handle || !str) {
		return false;
	}

	if (handle->IsMutable()) {
		return yyjson_mut_equals_str(handle->m_pVal_mut, str);
	} else {
		return yyjson_equals_str(handle->m_pVal, str);
	}
}

JsonValue* JsonManager::DeepCopy(JsonValue* targetDoc, JsonValue* sourceValue,
	char* error, size_t error_size)
{
	NativeErrorBuffer::Clear(error, error_size);

	if (!targetDoc || !sourceValue) {
		NativeErrorBuffer::Set(error, error_size, "Invalid parameters");
		return nullptr;
	}

	auto pJSONValue = CreateWrapper();

	if (targetDoc->IsMutable()) {
		pJSONValue->m_pDocument_mut = CreateDocument();
		if (!pJSONValue->m_pDocument_mut) {
			NativeErrorBuffer::Set(error, error_size, "Failed to create mutable document");
			return nullptr;
		}

		yyjson_mut_val* val_copy;
		if (sourceValue->IsMutable()) {
			val_copy = yyjson_mut_val_mut_copy(pJSONValue->m_pDocument_mut->get(), sourceValue->m_pVal_mut);
		} else {
			val_copy = yyjson_val_mut_copy(pJSONValue->m_pDocument_mut->get(), sourceValue->m_pVal);
		}

		if (!val_copy) {
			NativeErrorBuffer::Set(error, error_size, "Failed to copy value into mutable document");
			return nullptr;
		}

		yyjson_mut_doc_set_root(pJSONValue->m_pDocument_mut->get(), val_copy);
		pJSONValue->m_pVal_mut = val_copy;
	} else {
		yyjson_mut_doc* temp_doc = yyjson_mut_doc_new(nullptr);
		if (!temp_doc) {
			NativeErrorBuffer::Set(error, error_size, "Failed to create temporary document");
			return nullptr;
		}

		yyjson_mut_val* temp_val;
		if (sourceValue->IsMutable()) {
			temp_val = yyjson_mut_val_mut_copy(temp_doc, sourceValue->m_pVal_mut);
		} else {
			temp_val = yyjson_val_mut_copy(temp_doc, sourceValue->m_pVal);
		}

		if (!temp_val) {
			yyjson_mut_doc_free(temp_doc);
			NativeErrorBuffer::Set(error, error_size, "Failed to copy value into temporary document");
			return nullptr;
		}

		yyjson_mut_doc_set_root(temp_doc, temp_val);

		yyjson_doc* doc = yyjson_mut_doc_imut_copy(temp_doc, nullptr);
		yyjson_mut_doc_free(temp_doc);

		if (!doc) {
			NativeErrorBuffer::Set(error, error_size, "Failed to convert to immutable document");
			return nullptr;
		}

		pJSONValue->m_pDocument = WrapImmutableDocument(doc);
		pJSONValue->m_pVal = yyjson_doc_get_root(doc);
	}

	return pJSONValue.release();
}

const char* JsonManager::GetTypeDesc(JsonValue* handle)
{
	if (!handle) {
		return "invalid";
	}

	if (handle->IsMutable()) {
		return yyjson_mut_get_type_desc(handle->m_pVal_mut);
	} else {
		return yyjson_get_type_desc(handle->m_pVal);
	}
}

size_t JsonManager::GetSerializedSize(JsonValue* handle, yyjson_write_flag write_flg)
{
	if (!handle) {
		return 0;
	}

	size_t json_size;
	char* json_str;

	if (handle->IsMutable()) {
		json_str = yyjson_mut_val_write(handle->m_pVal_mut, write_flg, &json_size);
	} else {
		json_str = yyjson_val_write(handle->m_pVal, write_flg, &json_size);
	}

	if (json_str) {
		free(json_str);
		return json_size + 1;
	}

	return 0;
}

JsonValue* JsonManager::ToMutable(JsonValue* handle,
	char* error, size_t error_size)
{
	NativeErrorBuffer::Clear(error, error_size);

	if (!handle) {
		NativeErrorBuffer::Set(error, error_size, "Invalid handle");
		return nullptr;
	}

	if (handle->IsMutable()) {
		NativeErrorBuffer::Set(error, error_size, "Document is already mutable");
		return nullptr;
	}

	auto pJSONValue = CreateWrapper();
	pJSONValue->m_pDocument_mut = CopyDocument(handle->m_pDocument->get());
	if (!pJSONValue->m_pDocument_mut) {
		NativeErrorBuffer::Set(error, error_size, "Failed to copy document");
		return nullptr;
	}
	pJSONValue->m_pVal_mut = yyjson_mut_doc_get_root(pJSONValue->m_pDocument_mut->get());
	if (!pJSONValue->m_pVal_mut) {
		NativeErrorBuffer::Set(error, error_size, "Failed to get root from mutable document");
		return nullptr;
	}

	return pJSONValue.release();
}

JsonValue* JsonManager::ToImmutable(JsonValue* handle,
	char* error, size_t error_size)
{
	NativeErrorBuffer::Clear(error, error_size);

	if (!handle) {
		NativeErrorBuffer::Set(error, error_size, "Invalid handle");
		return nullptr;
	}

	if (!handle->IsMutable()) {
		NativeErrorBuffer::Set(error, error_size, "Document is already immutable");
		return nullptr;
	}

	auto pJSONValue = CreateWrapper();
	yyjson_doc* mdoc = yyjson_mut_doc_imut_copy(handle->m_pDocument_mut->get(), nullptr);
	if (!mdoc) {
		NativeErrorBuffer::Set(error, error_size, "Failed to convert to immutable document");
		return nullptr;
	}
	pJSONValue->m_pDocument = WrapImmutableDocument(mdoc);
	if (!pJSONValue->m_pDocument) {
		yyjson_doc_free(mdoc);
		NativeErrorBuffer::Set(error, error_size, "Failed to wrap immutable document");
		return nullptr;
	}
	pJSONValue->m_pVal = yyjson_doc_get_root(pJSONValue->m_pDocument->get());

	return pJSONValue.release();
}

yyjson_type JsonManager::GetType(JsonValue* handle)
{
	if (!handle) {
		return YYJSON_TYPE_NONE;
	}

	if (handle->IsMutable()) {
		return yyjson_mut_get_type(handle->m_pVal_mut);
	} else {
		return yyjson_get_type(handle->m_pVal);
	}
}

yyjson_subtype JsonManager::GetSubtype(JsonValue* handle)
{
	if (!handle) {
		return YYJSON_SUBTYPE_NONE;
	}

	if (handle->IsMutable()) {
		return yyjson_mut_get_subtype(handle->m_pVal_mut);
	} else {
		return yyjson_get_subtype(handle->m_pVal);
	}
}

bool JsonManager::IsArray(JsonValue* handle)
{
	return JsonTemplates::CheckTypeTemplate(handle, yyjson_mut_is_arr, yyjson_is_arr);
}

bool JsonManager::IsObject(JsonValue* handle)
{
	return JsonTemplates::CheckTypeTemplate(handle, yyjson_mut_is_obj, yyjson_is_obj);
}

bool JsonManager::IsInt(JsonValue* handle)
{
	return JsonTemplates::CheckTypeTemplate(handle, yyjson_mut_is_int, yyjson_is_int);
}

bool JsonManager::IsUint(JsonValue* handle)
{
	return JsonTemplates::CheckTypeTemplate(handle, yyjson_mut_is_uint, yyjson_is_uint);
}

bool JsonManager::IsSint(JsonValue* handle)
{
	return JsonTemplates::CheckTypeTemplate(handle, yyjson_mut_is_sint, yyjson_is_sint);
}

bool JsonManager::IsNum(JsonValue* handle)
{
	return JsonTemplates::CheckTypeTemplate(handle, yyjson_mut_is_num, yyjson_is_num);
}

bool JsonManager::IsBool(JsonValue* handle)
{
	return JsonTemplates::CheckTypeTemplate(handle, yyjson_mut_is_bool, yyjson_is_bool);
}

bool JsonManager::IsTrue(JsonValue* handle)
{
	return JsonTemplates::CheckTypeTemplate(handle, yyjson_mut_is_true, yyjson_is_true);
}

bool JsonManager::IsFalse(JsonValue* handle)
{
	return JsonTemplates::CheckTypeTemplate(handle, yyjson_mut_is_false, yyjson_is_false);
}

bool JsonManager::IsFloat(JsonValue* handle)
{
	return JsonTemplates::CheckTypeTemplate(handle, yyjson_mut_is_real, yyjson_is_real);
}

bool JsonManager::IsStr(JsonValue* handle)
{
	return JsonTemplates::CheckTypeTemplate(handle, yyjson_mut_is_str, yyjson_is_str);
}

bool JsonManager::IsNull(JsonValue* handle)
{
	return JsonTemplates::CheckTypeTemplate(handle, yyjson_mut_is_null, yyjson_is_null);
}

bool JsonManager::IsCtn(JsonValue* handle)
{
	if (!handle) {
		return false;
	}

	if (handle->IsMutable()) {
		return yyjson_mut_is_ctn(handle->m_pVal_mut);
	} else {
		return yyjson_is_ctn(handle->m_pVal);
	}
}

bool JsonManager::IsMutable(JsonValue* handle)
{
	if (!handle) {
		return false;
	}

	return handle->IsMutable();
}

bool JsonManager::IsImmutable(JsonValue* handle)
{
	if (!handle) {
		return false;
	}

	return handle->IsImmutable();
}

size_t JsonManager::GetReadSize(JsonValue* handle)
{
	if (!handle) {
		return 0;
	}

	if (handle->m_readSize == 0) {
		return 0;
	}

	return handle->m_readSize + 1;
}

size_t JsonManager::GetRefCount(JsonValue* handle)
{
	if (!handle) {
		return 0;
	}
	return handle->GetDocumentRefCount();
}

size_t JsonManager::GetValCount(JsonValue* handle)
{
	if (!handle || !handle->IsImmutable()) {
		return 0;
	}
	return yyjson_doc_get_val_count(handle->m_pDocument->get());
}

JsonValue* JsonManager::ObjectInit()
{
	auto pJSONValue = CreateWrapper();
	pJSONValue->m_pDocument_mut = CreateDocument();

	if (!pJSONValue->m_pDocument_mut) {
		return nullptr;
	}

	pJSONValue->m_pVal_mut = yyjson_mut_obj(pJSONValue->m_pDocument_mut->get());

	if (!pJSONValue->m_pVal_mut) {
		return nullptr;
	}

	yyjson_mut_doc_set_root(pJSONValue->m_pDocument_mut->get(), pJSONValue->m_pVal_mut);

	return pJSONValue.release();
}

JsonValue* JsonManager::ObjectInitWithStrings(const char** pairs, size_t count)
{
	if (!pairs || count == 0) {
		return nullptr;
	}

	auto pJSONValue = CreateWrapper();
	pJSONValue->m_pDocument_mut = CreateDocument();

	if (!pJSONValue->m_pDocument_mut) {
		return nullptr;
	}

	pJSONValue->m_pVal_mut = yyjson_mut_obj_with_kv(
		pJSONValue->m_pDocument_mut->get(),
		pairs,
		count
	);

	if (!pJSONValue->m_pVal_mut) {
		return nullptr;
	}

	yyjson_mut_doc_set_root(pJSONValue->m_pDocument_mut->get(), pJSONValue->m_pVal_mut);

	return pJSONValue.release();
}

JsonValue* JsonManager::ObjectParseString(const char* str, yyjson_read_flag read_flg,
	char* error, size_t error_size)
{
	return ParseTypedRootValue(str, false, read_flg, ContainerRootType::Object,
		"Invalid string",
		"Failed to parse JSON str: %s (error code: %u, position: %zu)",
		"Root value is not an object (got %s)",
		"Root value in file is not an object (got %s)",
		error, error_size);
}

JsonValue* JsonManager::ObjectParseFile(const char* path, yyjson_read_flag read_flg,
	char* error, size_t error_size)
{
	return ParseTypedRootValue(path, true, read_flg, ContainerRootType::Object,
		"Invalid path",
		"Failed to parse JSON str: %s (error code: %u, position: %zu)",
		"Root value is not an object (got %s)",
		"Root value in file is not an object (got %s)",
		error, error_size);
}

size_t JsonManager::ObjectGetSize(JsonValue* handle)
{
	if (!handle) {
		return 0;
	}

	if (handle->IsMutable()) {
		return yyjson_mut_obj_size(handle->m_pVal_mut);
	} else {
		return yyjson_obj_size(handle->m_pVal);
	}
}

bool JsonManager::ObjectGetKey(JsonValue* handle, size_t index, const char** out_key)
{
	if (!handle || !out_key) {
		return false;
	}

	if (handle->IsMutable()) {
		size_t obj_size = yyjson_mut_obj_size(handle->m_pVal_mut);
		if (index >= obj_size) {
			return false;
		}

		yyjson_mut_obj_iter iter;
		yyjson_mut_obj_iter_init(handle->m_pVal_mut, &iter);

		for (size_t i = 0; i < index; i++) {
			yyjson_mut_obj_iter_next(&iter);
		}

		yyjson_mut_val* key = yyjson_mut_obj_iter_next(&iter);
		if (!key) {
			return false;
		}

		*out_key = yyjson_mut_get_str(key);
		return true;
	} else {
		size_t obj_size = yyjson_obj_size(handle->m_pVal);
		if (index >= obj_size) {
			return false;
		}

		yyjson_obj_iter iter;
		yyjson_obj_iter_init(handle->m_pVal, &iter);

		for (size_t i = 0; i < index; i++) {
			yyjson_obj_iter_next(&iter);
		}

		yyjson_val* key = yyjson_obj_iter_next(&iter);
		if (!key) {
			return false;
		}

		*out_key = yyjson_get_str(key);
		return true;
	}
}

JsonValue* JsonManager::ObjectGetValueAt(JsonValue* handle, size_t index)
{
	if (!handle) {
		return nullptr;
	}

	size_t obj_size = handle->IsMutable() ? yyjson_mut_obj_size(handle->m_pVal_mut) : yyjson_obj_size(handle->m_pVal);

	if (index >= obj_size) {
		return nullptr;
	}

	auto pJSONValue = CreateWrapper();

	if (handle->IsMutable()) {
		yyjson_mut_obj_iter iter;
		yyjson_mut_obj_iter_init(handle->m_pVal_mut, &iter);

		for (size_t i = 0; i < index; i++) {
			yyjson_mut_obj_iter_next(&iter);
		}

		yyjson_mut_val* key = yyjson_mut_obj_iter_next(&iter);
		if (!key) {
			return nullptr;
		}

		pJSONValue->m_pDocument_mut = handle->m_pDocument_mut;
		pJSONValue->m_pVal_mut = yyjson_mut_obj_iter_get_val(key);
	} else {
		yyjson_obj_iter iter;
		yyjson_obj_iter_init(handle->m_pVal, &iter);

		yyjson_val* key;
		for (size_t i = 0; i <= index; i++) {
			key = yyjson_obj_iter_next(&iter);
			if (!key) {
				return nullptr;
			}
		}

		pJSONValue->m_pDocument = handle->m_pDocument;
		pJSONValue->m_pVal = yyjson_obj_iter_get_val(key);
	}

	return pJSONValue.release();
}

JsonValue* JsonManager::ObjectGet(JsonValue* handle, const char* key)
{
	if (!handle || !key) {
		return nullptr;
	}

	auto pJSONValue = CreateWrapper();

	if (handle->IsMutable()) {
		yyjson_mut_val* val = yyjson_mut_obj_get(handle->m_pVal_mut, key);
		if (!val) {
			return nullptr;
		}

		pJSONValue->m_pDocument_mut = handle->m_pDocument_mut;
		pJSONValue->m_pVal_mut = val;
	} else {
		yyjson_val* val = yyjson_obj_get(handle->m_pVal, key);
		if (!val) {
			return nullptr;
		}

		pJSONValue->m_pDocument = handle->m_pDocument;
		pJSONValue->m_pVal = val;
	}

	return pJSONValue.release();
}

bool JsonManager::ObjectGetBool(JsonValue* handle, const char* key, bool* out_value)
{
	if (!key) {
		return false;
	}

	return JsonTemplates::GetValueTemplate<bool>(
		handle,
		[&]() { return yyjson_mut_obj_get(handle->m_pVal_mut, key); },
		[&]() { return yyjson_obj_get(handle->m_pVal, key); },
		out_value
	);
}

bool JsonManager::ObjectGetDouble(JsonValue* handle, const char* key, double* out_value)
{
	if (!key) {
		return false;
	}

	return JsonTemplates::GetValueTemplate<double>(
		handle,
		[&]() { return yyjson_mut_obj_get(handle->m_pVal_mut, key); },
		[&]() { return yyjson_obj_get(handle->m_pVal, key); },
		out_value
	);
}

bool JsonManager::ObjectGetInt(JsonValue* handle, const char* key, int* out_value)
{
	if (!key) {
		return false;
	}

	return JsonTemplates::GetValueTemplate<int>(
		handle,
		[&]() { return yyjson_mut_obj_get(handle->m_pVal_mut, key); },
		[&]() { return yyjson_obj_get(handle->m_pVal, key); },
		out_value
	);
}

bool JsonManager::ObjectGetInt64(JsonValue* handle, const char* key, int64_t* out_value)
{
	if (!handle || !key || !out_value) {
		return false;
	}

	if (handle->IsMutable()) {
		yyjson_mut_val* val = yyjson_mut_obj_get(handle->m_pVal_mut, key);
		if (!val) {
			return false;
		}
		return ReadInt64FromMutVal(val, out_value);
	} else {
		yyjson_val* val = yyjson_obj_get(handle->m_pVal, key);
		if (!val) {
			return false;
		}
		return ReadInt64FromVal(val, out_value);
	}
}

bool JsonManager::ObjectGetUint64(JsonValue* handle, const char* key, uint64_t* out_value)
{
	if (!handle || !key || !out_value) {
		return false;
	}

	if (handle->IsMutable()) {
		yyjson_mut_val* val = yyjson_mut_obj_get(handle->m_pVal_mut, key);
		return val && ReadUint64FromMutVal(val, out_value);
	}

	yyjson_val* val = yyjson_obj_get(handle->m_pVal, key);
	return val && ReadUint64FromVal(val, out_value);
}

bool JsonManager::ObjectGetString(JsonValue* handle, const char* key, const char** out_str, size_t* out_len)
{
	if (!handle || !key || !out_str) {
		return false;
	}

	if (handle->IsMutable()) {
		yyjson_mut_val* val = yyjson_mut_obj_get(handle->m_pVal_mut, key);
		if (!val || !yyjson_mut_is_str(val)) {
			return false;
		}

		*out_str = yyjson_mut_get_str(val);
		if (out_len) {
			*out_len = yyjson_mut_get_len(val);
		}
		return true;
	} else {
		yyjson_val* val = yyjson_obj_get(handle->m_pVal, key);
		if (!val || !yyjson_is_str(val)) {
			return false;
		}

		*out_str = yyjson_get_str(val);
		if (out_len) {
			*out_len = yyjson_get_len(val);
		}
		return true;
	}
}

bool JsonManager::ObjectIsNull(JsonValue* handle, const char* key, bool* out_is_null)
{
	if (!handle || !key || !out_is_null) {
		return false;
	}

	if (handle->IsMutable()) {
		yyjson_mut_val* val = yyjson_mut_obj_get(handle->m_pVal_mut, key);
		if (!val) {
			return false;
		}

		*out_is_null = yyjson_mut_is_null(val);
		return true;
	} else {
		yyjson_val* val = yyjson_obj_get(handle->m_pVal, key);
		if (!val) {
			return false;
		}

		*out_is_null = yyjson_is_null(val);
		return true;
	}
}

bool JsonManager::ObjectHasKey(JsonValue* handle, const char* key, bool use_pointer)
{
	if (!handle || !key) {
		return false;
	}

	if (handle->IsMutable()) {
		if (use_pointer) {
			return yyjson_mut_doc_ptr_get(handle->m_pDocument_mut->get(), key) != nullptr;
		} else {
			yyjson_mut_obj_iter iter = yyjson_mut_obj_iter_with(handle->m_pVal_mut);
			return yyjson_mut_obj_iter_get(&iter, key) != nullptr;
		}
	} else {
		if (use_pointer) {
			return yyjson_doc_ptr_get(handle->m_pDocument->get(), key) != nullptr;
		} else {
			yyjson_obj_iter iter = yyjson_obj_iter_with(handle->m_pVal);
			return yyjson_obj_iter_get(&iter, key) != nullptr;
		}
	}
}

bool JsonManager::ObjectRenameKey(JsonValue* handle, const char* old_key, const char* new_key, bool allow_duplicate)
{
	if (!handle || !handle->IsMutable() || !old_key || !new_key) {
		return false;
	}

	if (!yyjson_mut_obj_get(handle->m_pVal_mut, old_key)) {
		return false;
	}

	if (!allow_duplicate && yyjson_mut_obj_get(handle->m_pVal_mut, new_key)) {
		return false;
	}

	return yyjson_mut_obj_rename_key(handle->m_pDocument_mut->get(), handle->m_pVal_mut, old_key, new_key);
}

bool JsonManager::ObjectSet(JsonValue* handle, const char* key, JsonValue* value)
{
	if (!handle || !handle->IsMutable() || !key || !value) {
		return false;
	}

	yyjson_mut_val* val_copy;
	if (value->IsMutable()) {
		val_copy = yyjson_mut_val_mut_copy(handle->m_pDocument_mut->get(), value->m_pVal_mut);
	} else {
		val_copy = yyjson_val_mut_copy(handle->m_pDocument_mut->get(), value->m_pVal);
	}

	if (!val_copy) {
		return false;
	}

	return yyjson_mut_obj_put(handle->m_pVal_mut, yyjson_mut_strcpy(handle->m_pDocument_mut->get(), key), val_copy);
}

bool JsonManager::ObjectSetBool(JsonValue* handle, const char* key, bool value)
{
	return JsonTemplates::ObjectSetTemplate<bool>(handle, key, value);
}

bool JsonManager::ObjectSetDouble(JsonValue* handle, const char* key, double value)
{
	return JsonTemplates::ObjectSetTemplate<double>(handle, key, value);
}

bool JsonManager::ObjectSetInt(JsonValue* handle, const char* key, int value)
{
	return JsonTemplates::ObjectSetTemplate<int>(handle, key, value);
}

bool JsonManager::ObjectSetInt64(JsonValue* handle, const char* key, int64_t value)
{
	if (!handle || !handle->IsMutable() || !key) {
		return false;
	}

	yyjson_mut_doc* doc = handle->m_pDocument_mut->get();
	return yyjson_mut_obj_put(handle->m_pVal_mut, yyjson_mut_strcpy(doc, key),
		yyjson_mut_sint(doc, value));
}

bool JsonManager::ObjectSetUint64(JsonValue* handle, const char* key, uint64_t value)
{
	if (!handle || !handle->IsMutable() || !key) {
		return false;
	}

	yyjson_mut_doc* doc = handle->m_pDocument_mut->get();
	return yyjson_mut_obj_put(handle->m_pVal_mut, yyjson_mut_strcpy(doc, key),
		yyjson_mut_uint(doc, value));
}

bool JsonManager::ObjectSetNull(JsonValue* handle, const char* key)
{
	if (!handle || !handle->IsMutable() || !key) {
		return false;
	}

	yyjson_mut_doc* doc = handle->m_pDocument_mut->get();
	return yyjson_mut_obj_put(
		handle->m_pVal_mut,
		yyjson_mut_strcpy(doc, key),
		yyjson_mut_null(doc)
	);
}

bool JsonManager::ObjectSetString(JsonValue* handle, const char* key, const char* value)
{
	if (!handle || !handle->IsMutable() || !key || !value) {
		return false;
	}

	yyjson_mut_doc* doc = handle->m_pDocument_mut->get();
	return yyjson_mut_obj_put(
		handle->m_pVal_mut,
		yyjson_mut_strcpy(doc, key),
		yyjson_mut_strcpy(doc, value)
	);
}

bool JsonManager::ObjectRemove(JsonValue* handle, const char* key)
{
	if (!handle || !handle->IsMutable() || !key) {
		return false;
	}

	return yyjson_mut_obj_remove_key(handle->m_pVal_mut, key) != nullptr;
}

bool JsonManager::ObjectClear(JsonValue* handle)
{
	if (!handle || !handle->IsMutable()) {
		return false;
	}

	return yyjson_mut_obj_clear(handle->m_pVal_mut);
}

bool JsonManager::ObjectSort(JsonValue* handle, JSON_SORT_ORDER sort_mode)
{
	if (!handle || !handle->IsMutable()) {
		return false;
	}

	if (!yyjson_mut_is_obj(handle->m_pVal_mut)) {
		return false;
	}

	if (sort_mode < JSON_SORT_ASC || sort_mode > JSON_SORT_RANDOM) {
		return false;
	}

	size_t obj_size = yyjson_mut_obj_size(handle->m_pVal_mut);
	if (obj_size <= 1) return true;

	struct KeyValuePair {
		yyjson_mut_val* key;
		const char* key_str;
		size_t key_len;
		yyjson_mut_val* val;
	};
	std::vector<KeyValuePair> pairs;
	pairs.reserve(obj_size);

	size_t idx, max;
	yyjson_mut_val *key, *val;
	yyjson_mut_obj_foreach(handle->m_pVal_mut, idx, max, key, val) {
		const char* key_str = yyjson_mut_get_str(key);
		size_t key_len = yyjson_mut_get_len(key);
		pairs.push_back({key, key_str, key_len, val});
	}

	if (sort_mode == JSON_SORT_RANDOM) {
		std::shuffle(pairs.begin(), pairs.end(), m_randomGenerator);
	}
	else {
		auto compare = [sort_mode](const KeyValuePair& a, const KeyValuePair& b) {
			size_t min_len = a.key_len < b.key_len ? a.key_len : b.key_len;
			int cmp = memcmp(a.key_str, b.key_str, min_len);
			if (cmp == 0) {
				cmp = (a.key_len < b.key_len) ? -1 : (a.key_len > b.key_len ? 1 : 0);
			}
			return sort_mode == JSON_SORT_ASC ? cmp < 0 : cmp > 0;
		};

		std::sort(pairs.begin(), pairs.end(), compare);
	}

	yyjson_mut_obj_clear(handle->m_pVal_mut);

	for (const auto& pair : pairs) {
		yyjson_mut_obj_add(handle->m_pVal_mut, pair.key, pair.val);
	}

	return true;
}

bool JsonManager::ObjectRotate(JsonValue* handle, size_t idx)
{
	if (!handle || !handle->IsMutable()) {
		return false;
	}

	if (!yyjson_mut_is_obj(handle->m_pVal_mut)) {
		return false;
	}

	return yyjson_mut_obj_rotate(handle->m_pVal_mut, idx);
}

JsonValue* JsonManager::ArrayInit()
{
	auto pJSONValue = CreateWrapper();
	pJSONValue->m_pDocument_mut = CreateDocument();

	if (!pJSONValue->m_pDocument_mut) {
		return nullptr;
	}

	pJSONValue->m_pVal_mut = yyjson_mut_arr(pJSONValue->m_pDocument_mut->get());

	if (!pJSONValue->m_pVal_mut) {
		return nullptr;
	}

	yyjson_mut_doc_set_root(pJSONValue->m_pDocument_mut->get(), pJSONValue->m_pVal_mut);

	return pJSONValue.release();
}

JsonValue* JsonManager::ArrayInitWithStrings(const char** strings, size_t count)
{
	if (count == 0) {
		return ArrayInit();
	}

	if (!strings) {
		return nullptr;
	}

	auto pJSONValue = CreateWrapper();
	pJSONValue->m_pDocument_mut = CreateDocument();

	if (!pJSONValue->m_pDocument_mut) {
		return nullptr;
	}

	pJSONValue->m_pVal_mut = yyjson_mut_arr_with_strcpy(
		pJSONValue->m_pDocument_mut->get(),
		strings,
		count
	);

	if (!pJSONValue->m_pVal_mut) {
		return nullptr;
	}

	yyjson_mut_doc_set_root(pJSONValue->m_pDocument_mut->get(), pJSONValue->m_pVal_mut);

	return pJSONValue.release();
}

JsonValue* JsonManager::ArrayInitWithInt32(const int32_t* values, size_t count,
	char* error, size_t error_size)
{
	NativeErrorBuffer::Clear(error, error_size);

	if (count == 0) {
		return ArrayInit();
	}

	if (!values) {
		NativeErrorBuffer::Set(error, error_size, "Invalid values parameter");
		return nullptr;
	}

	auto pJSONValue = CreateWrapper();
	pJSONValue->m_pDocument_mut = CreateDocument();

	if (!pJSONValue->m_pDocument_mut) {
		NativeErrorBuffer::Set(error, error_size, "Failed to create document");
		return nullptr;
	}

	pJSONValue->m_pVal_mut = yyjson_mut_arr_with_sint32(
		pJSONValue->m_pDocument_mut->get(),
		values,
		count
	);

	if (!pJSONValue->m_pVal_mut) {
		NativeErrorBuffer::Set(error, error_size, "Failed to create array from int32 values");
		return nullptr;
	}

	yyjson_mut_doc_set_root(pJSONValue->m_pDocument_mut->get(), pJSONValue->m_pVal_mut);

	return pJSONValue.release();
}

JsonValue* JsonManager::ArrayInitWithInt64(const char** values, size_t count, char* error, size_t error_size)
{
	NativeErrorBuffer::Clear(error, error_size);

	if (count == 0) {
		return ArrayInit();
	}

	if (!values) {
		NativeErrorBuffer::Set(error, error_size, "Invalid values parameter");
		return nullptr;
	}

	auto pJSONValue = CreateWrapper();
	pJSONValue->m_pDocument_mut = CreateDocument();

	if (!pJSONValue->m_pDocument_mut) {
		NativeErrorBuffer::Set(error, error_size, "Failed to create document");
		return nullptr;
	}

	auto doc = pJSONValue->m_pDocument_mut->get();
	pJSONValue->m_pVal_mut = yyjson_mut_arr(doc);

	if (!pJSONValue->m_pVal_mut) {
		NativeErrorBuffer::Set(error, error_size, "Failed to create array");
		return nullptr;
	}

	for (size_t i = 0; i < count; i++) {
		std::variant<int64_t, uint64_t> variant_value;
		if (!ParseInt64Variant(values[i], &variant_value, error, error_size)) {
			return nullptr;
		}

		yyjson_mut_val* val;
		if (std::holds_alternative<int64_t>(variant_value)) {
			val = yyjson_mut_sint(doc, std::get<int64_t>(variant_value));
		} else {
			val = yyjson_mut_uint(doc, std::get<uint64_t>(variant_value));
		}

		if (!val || !yyjson_mut_arr_append(pJSONValue->m_pVal_mut, val)) {
			NativeErrorBuffer::Set(error, error_size, "Failed to append value at index %zu", i);
			return nullptr;
		}
	}

	yyjson_mut_doc_set_root(pJSONValue->m_pDocument_mut->get(), pJSONValue->m_pVal_mut);

	return pJSONValue.release();
}

JsonValue* JsonManager::ArrayInitWithBool(const bool* values, size_t count,
	char* error, size_t error_size)
{
	NativeErrorBuffer::Clear(error, error_size);

	if (count == 0) {
		return ArrayInit();
	}

	if (!values) {
		NativeErrorBuffer::Set(error, error_size, "Invalid values parameter");
		return nullptr;
	}

	auto pJSONValue = CreateWrapper();
	pJSONValue->m_pDocument_mut = CreateDocument();

	if (!pJSONValue->m_pDocument_mut) {
		NativeErrorBuffer::Set(error, error_size, "Failed to create document");
		return nullptr;
	}

	pJSONValue->m_pVal_mut = yyjson_mut_arr_with_bool(
		pJSONValue->m_pDocument_mut->get(),
		values,
		count
	);

	if (!pJSONValue->m_pVal_mut) {
		NativeErrorBuffer::Set(error, error_size, "Failed to create array from bool values");
		return nullptr;
	}

	yyjson_mut_doc_set_root(pJSONValue->m_pDocument_mut->get(), pJSONValue->m_pVal_mut);

	return pJSONValue.release();
}

JsonValue* JsonManager::ArrayInitWithDouble(const double* values, size_t count,
	char* error, size_t error_size)
{
	NativeErrorBuffer::Clear(error, error_size);

	if (count == 0) {
		return ArrayInit();
	}

	if (!values) {
		NativeErrorBuffer::Set(error, error_size, "Invalid values parameter");
		return nullptr;
	}

	auto pJSONValue = CreateWrapper();
	pJSONValue->m_pDocument_mut = CreateDocument();

	if (!pJSONValue->m_pDocument_mut) {
		NativeErrorBuffer::Set(error, error_size, "Failed to create document");
		return nullptr;
	}

	pJSONValue->m_pVal_mut = yyjson_mut_arr_with_real(
		pJSONValue->m_pDocument_mut->get(),
		values,
		count
	);

	if (!pJSONValue->m_pVal_mut) {
		NativeErrorBuffer::Set(error, error_size, "Failed to create array from float values");
		return nullptr;
	}

	yyjson_mut_doc_set_root(pJSONValue->m_pDocument_mut->get(), pJSONValue->m_pVal_mut);

	return pJSONValue.release();
}

JsonValue* JsonManager::ArrayParseString(const char* str, yyjson_read_flag read_flg,
	char* error, size_t error_size)
{
	return ParseTypedRootValue(str, false, read_flg, ContainerRootType::Array,
		"Invalid string",
		"Failed to parse JSON string: %s (error code: %u, position: %zu)",
		"Root value is not an array (got %s)",
		"Root value in file is not an array (got %s)",
		error, error_size);
}

JsonValue* JsonManager::ArrayParseFile(const char* path, yyjson_read_flag read_flg,
	char* error, size_t error_size)
{
	return ParseTypedRootValue(path, true, read_flg, ContainerRootType::Array,
		"Invalid path",
		"Failed to parse JSON string: %s (error code: %u, position: %zu)",
		"Root value is not an array (got %s)",
		"Root value in file is not an array (got %s)",
		error, error_size);
}

size_t JsonManager::ArrayGetSize(JsonValue* handle)
{
	if (!handle) {
		return 0;
	}

	if (handle->IsMutable()) {
		return yyjson_mut_arr_size(handle->m_pVal_mut);
	} else {
		return yyjson_arr_size(handle->m_pVal);
	}
}

JsonValue* JsonManager::ArrayGet(JsonValue* handle, size_t index)
{
	if (!handle) {
		return nullptr;
	}

	auto pJSONValue = CreateWrapper();

	if (handle->IsMutable()) {
		size_t arr_size = yyjson_mut_arr_size(handle->m_pVal_mut);
		if (index >= arr_size) {
			return nullptr;
		}

		yyjson_mut_val* val = yyjson_mut_arr_get(handle->m_pVal_mut, index);
		if (!val) {
			return nullptr;
		}

		pJSONValue->m_pDocument_mut = handle->m_pDocument_mut;
		pJSONValue->m_pVal_mut = val;
	} else {
		size_t arr_size = yyjson_arr_size(handle->m_pVal);
		if (index >= arr_size) {
			return nullptr;
		}

		yyjson_val* val = yyjson_arr_get(handle->m_pVal, index);
		if (!val) {
			return nullptr;
		}

		pJSONValue->m_pDocument = handle->m_pDocument;
		pJSONValue->m_pVal = val;
	}

	return pJSONValue.release();
}

JsonValue* JsonManager::ArrayGetFirst(JsonValue* handle)
{
	if (!handle) {
		return nullptr;
	}

	auto pJSONValue = CreateWrapper();

	if (handle->IsMutable()) {
		size_t arr_size = yyjson_mut_arr_size(handle->m_pVal_mut);
		if (arr_size == 0) {
			return nullptr;
		}

		yyjson_mut_val* val = yyjson_mut_arr_get_first(handle->m_pVal_mut);
		if (!val) {
			return nullptr;
		}

		pJSONValue->m_pDocument_mut = handle->m_pDocument_mut;
		pJSONValue->m_pVal_mut = val;
	} else {
		size_t arr_size = yyjson_arr_size(handle->m_pVal);
		if (arr_size == 0) {
			return nullptr;
		}

		yyjson_val* val = yyjson_arr_get_first(handle->m_pVal);
		if (!val) {
			return nullptr;
		}

		pJSONValue->m_pDocument = handle->m_pDocument;
		pJSONValue->m_pVal = val;
	}

	return pJSONValue.release();
}

JsonValue* JsonManager::ArrayGetLast(JsonValue* handle)
{
	if (!handle) {
		return nullptr;
	}

	auto pJSONValue = CreateWrapper();

	if (handle->IsMutable()) {
		size_t arr_size = yyjson_mut_arr_size(handle->m_pVal_mut);
		if (arr_size == 0) {
			return nullptr;
		}

		yyjson_mut_val* val = yyjson_mut_arr_get_last(handle->m_pVal_mut);
		if (!val) {
			return nullptr;
		}

		pJSONValue->m_pDocument_mut = handle->m_pDocument_mut;
		pJSONValue->m_pVal_mut = val;
	} else {
		size_t arr_size = yyjson_arr_size(handle->m_pVal);
		if (arr_size == 0) {
			return nullptr;
		}

		yyjson_val* val = yyjson_arr_get_last(handle->m_pVal);
		if (!val) {
			return nullptr;
		}

		pJSONValue->m_pDocument = handle->m_pDocument;
		pJSONValue->m_pVal = val;
	}

	return pJSONValue.release();
}

bool JsonManager::ArrayGetBool(JsonValue* handle, size_t index, bool* out_value)
{
	if (!handle || !out_value) {
		return false;
	}

	if (handle->IsMutable()) {
		size_t arr_size = yyjson_mut_arr_size(handle->m_pVal_mut);
		if (index >= arr_size) {
			return false;
		}

		yyjson_mut_val* val = yyjson_mut_arr_get(handle->m_pVal_mut, index);
		if (!yyjson_mut_is_bool(val)) {
			return false;
		}

		*out_value = yyjson_mut_get_bool(val);
		return true;
	} else {
		size_t arr_size = yyjson_arr_size(handle->m_pVal);
		if (index >= arr_size) {
			return false;
		}

		yyjson_val* val = yyjson_arr_get(handle->m_pVal, index);
		if (!yyjson_is_bool(val)) {
			return false;
		}

		*out_value = yyjson_get_bool(val);
		return true;
	}
}

bool JsonManager::ArrayGetDouble(JsonValue* handle, size_t index, double* out_value)
{
	if (!handle || !out_value) {
		return false;
	}

	if (handle->IsMutable()) {
		size_t arr_size = yyjson_mut_arr_size(handle->m_pVal_mut);
		if (index >= arr_size) {
			return false;
		}

		yyjson_mut_val* val = yyjson_mut_arr_get(handle->m_pVal_mut, index);
		if (!yyjson_mut_is_num(val)) {
			return false;
		}

		*out_value = yyjson_mut_get_num(val);
		return true;
	} else {
		size_t arr_size = yyjson_arr_size(handle->m_pVal);
		if (index >= arr_size) {
			return false;
		}

		yyjson_val* val = yyjson_arr_get(handle->m_pVal, index);
		if (!yyjson_is_num(val)) {
			return false;
		}

		*out_value = yyjson_get_num(val);
		return true;
	}
}

bool JsonManager::ArrayGetInt(JsonValue* handle, size_t index, int* out_value)
{
	if (!handle || !out_value) {
		return false;
	}

	if (handle->IsMutable()) {
		size_t arr_size = yyjson_mut_arr_size(handle->m_pVal_mut);
		if (index >= arr_size) {
			return false;
		}

		yyjson_mut_val* val = yyjson_mut_arr_get(handle->m_pVal_mut, index);
		if (!yyjson_mut_is_int(val)) {
			return false;
		}

		*out_value = yyjson_mut_get_int(val);
		return true;
	} else {
		size_t arr_size = yyjson_arr_size(handle->m_pVal);
		if (index >= arr_size) {
			return false;
		}

		yyjson_val* val = yyjson_arr_get(handle->m_pVal, index);
		if (!yyjson_is_int(val)) {
			return false;
		}

		*out_value = yyjson_get_int(val);
		return true;
	}
}

bool JsonManager::ArrayGetInt64(JsonValue* handle, size_t index, int64_t* out_value)
{
	if (!handle || !out_value) {
		return false;
	}

	if (handle->IsMutable()) {
		size_t arr_size = yyjson_mut_arr_size(handle->m_pVal_mut);
		if (index >= arr_size) {
			return false;
		}

		yyjson_mut_val* val = yyjson_mut_arr_get(handle->m_pVal_mut, index);
		return ReadInt64FromMutVal(val, out_value);
	} else {
		size_t arr_size = yyjson_arr_size(handle->m_pVal);
		if (index >= arr_size) {
			return false;
		}

		yyjson_val* val = yyjson_arr_get(handle->m_pVal, index);
		return ReadInt64FromVal(val, out_value);
	}
}

bool JsonManager::ArrayGetUint64(JsonValue* handle, size_t index, uint64_t* out_value)
{
	if (!handle || !out_value) return false;
	if (handle->IsMutable()) {
		if (index >= yyjson_mut_arr_size(handle->m_pVal_mut)) return false;
		return ReadUint64FromMutVal(yyjson_mut_arr_get(handle->m_pVal_mut, index), out_value);
	}
	if (index >= yyjson_arr_size(handle->m_pVal)) return false;
	return ReadUint64FromVal(yyjson_arr_get(handle->m_pVal, index), out_value);
}

bool JsonManager::ArrayGetString(JsonValue* handle, size_t index, const char** out_str, size_t* out_len)
{
	if (!handle || !out_str) {
		return false;
	}

	if (handle->IsMutable()) {
		size_t arr_size = yyjson_mut_arr_size(handle->m_pVal_mut);
		if (index >= arr_size) {
			return false;
		}

		yyjson_mut_val* val = yyjson_mut_arr_get(handle->m_pVal_mut, index);
		if (!yyjson_mut_is_str(val)) {
			return false;
		}

		*out_str = yyjson_mut_get_str(val);
		if (out_len) {
			*out_len = yyjson_mut_get_len(val);
		}
		return true;
	} else {
		size_t arr_size = yyjson_arr_size(handle->m_pVal);
		if (index >= arr_size) {
			return false;
		}

		yyjson_val* val = yyjson_arr_get(handle->m_pVal, index);
		if (!yyjson_is_str(val)) {
			return false;
		}

		*out_str = yyjson_get_str(val);
		if (out_len) {
			*out_len = yyjson_get_len(val);
		}
		return true;
	}
}

bool JsonManager::ArrayIsNull(JsonValue* handle, size_t index)
{
	if (!handle) {
		return false;
	}

	if (handle->IsMutable()) {
		size_t arr_size = yyjson_mut_arr_size(handle->m_pVal_mut);
		if (index >= arr_size) {
			return false;
		}

		yyjson_mut_val* val = yyjson_mut_arr_get(handle->m_pVal_mut, index);
		return yyjson_mut_is_null(val);
	} else {
		size_t arr_size = yyjson_arr_size(handle->m_pVal);
		if (index >= arr_size) {
			return false;
		}

		yyjson_val* val = yyjson_arr_get(handle->m_pVal, index);
		return yyjson_is_null(val);
	}
}

bool JsonManager::ArrayReplace(JsonValue* handle, size_t index, JsonValue* value)
{
	if (!handle || !handle->IsMutable() || !value) {
		return false;
	}

	size_t arr_size = yyjson_mut_arr_size(handle->m_pVal_mut);
	if (index >= arr_size) {
		return false;
	}

	yyjson_mut_val* val_copy;
	if (value->IsMutable()) {
		val_copy = yyjson_mut_val_mut_copy(handle->m_pDocument_mut->get(), value->m_pVal_mut);
	} else {
		val_copy = yyjson_val_mut_copy(handle->m_pDocument_mut->get(), value->m_pVal);
	}

	if (!val_copy) {
		return false;
	}

	return yyjson_mut_arr_replace(handle->m_pVal_mut, index, val_copy) != nullptr;
}

bool JsonManager::ArrayReplaceBool(JsonValue* handle, size_t index, bool value)
{
	return JsonTemplates::ArrayReplaceTemplate<bool>(handle, index, value);
}

bool JsonManager::ArrayReplaceDouble(JsonValue* handle, size_t index, double value)
{
	return JsonTemplates::ArrayReplaceTemplate<double>(handle, index, value);
}

bool JsonManager::ArrayReplaceInt(JsonValue* handle, size_t index, int value)
{
	return JsonTemplates::ArrayReplaceTemplate<int>(handle, index, value);
}

bool JsonManager::ArrayReplaceInt64(JsonValue* handle, size_t index, int64_t value)
{
	if (!handle || !handle->IsMutable()) {
		return false;
	}

	size_t arr_size = yyjson_mut_arr_size(handle->m_pVal_mut);
	if (index >= arr_size) {
		return false;
	}

	return yyjson_mut_arr_replace(handle->m_pVal_mut, index,
		yyjson_mut_sint(handle->m_pDocument_mut->get(), value)) != nullptr;
}

bool JsonManager::ArrayReplaceUint64(JsonValue* handle, size_t index, uint64_t value)
{
	if (!handle || !handle->IsMutable() || index >= yyjson_mut_arr_size(handle->m_pVal_mut)) return false;
	return yyjson_mut_arr_replace(handle->m_pVal_mut, index,
		yyjson_mut_uint(handle->m_pDocument_mut->get(), value)) != nullptr;
}

bool JsonManager::ArrayReplaceNull(JsonValue* handle, size_t index)
{
	return JsonTemplates::ArrayReplaceNull(handle, index);
}

bool JsonManager::ArrayReplaceString(JsonValue* handle, size_t index, const char* value)
{
	return JsonTemplates::ArrayReplaceString(handle, index, value);
}

bool JsonManager::ArrayAppend(JsonValue* handle, JsonValue* value)
{
	if (!handle || !handle->IsMutable() || !value) {
		return false;
	}

	yyjson_mut_val* val_copy;
	if (value->IsMutable()) {
		val_copy = yyjson_mut_val_mut_copy(handle->m_pDocument_mut->get(), value->m_pVal_mut);
	} else {
		val_copy = yyjson_val_mut_copy(handle->m_pDocument_mut->get(), value->m_pVal);
	}

	if (!val_copy) {
		return false;
	}

	return yyjson_mut_arr_append(handle->m_pVal_mut, val_copy);
}

bool JsonManager::ArrayAppendBool(JsonValue* handle, bool value)
{
	return JsonTemplates::ArrayAppendTemplate<bool>(handle, value);
}

bool JsonManager::ArrayAppendDouble(JsonValue* handle, double value)
{
	return JsonTemplates::ArrayAppendTemplate<double>(handle, value);
}

bool JsonManager::ArrayAppendInt(JsonValue* handle, int value)
{
	return JsonTemplates::ArrayAppendTemplate<int>(handle, value);
}

bool JsonManager::ArrayAppendInt64(JsonValue* handle, int64_t value)
{
	if (!handle || !handle->IsMutable()) {
		return false;
	}

	return yyjson_mut_arr_append(handle->m_pVal_mut,
		yyjson_mut_sint(handle->m_pDocument_mut->get(), value));
}

bool JsonManager::ArrayAppendUint64(JsonValue* handle, uint64_t value)
{
	return handle && handle->IsMutable() && yyjson_mut_arr_append(handle->m_pVal_mut,
		yyjson_mut_uint(handle->m_pDocument_mut->get(), value));
}

bool JsonManager::ArrayAppendNull(JsonValue* handle)
{
	return JsonTemplates::ArrayAppendNull(handle);
}

bool JsonManager::ArrayAppendString(JsonValue* handle, const char* value)
{
	return JsonTemplates::ArrayAppendString(handle, value);
}

bool JsonManager::ArrayInsert(JsonValue* handle, size_t index, JsonValue* value)
{
	if (!handle || !handle->IsMutable() || !value) {
		return false;
	}

	size_t arr_size = yyjson_mut_arr_size(handle->m_pVal_mut);
	if (index > arr_size) {
		return false;
	}

	yyjson_mut_val* val_copy;
	if (value->IsMutable()) {
		val_copy = yyjson_mut_val_mut_copy(handle->m_pDocument_mut->get(), value->m_pVal_mut);
	} else {
		val_copy = yyjson_val_mut_copy(handle->m_pDocument_mut->get(), value->m_pVal);
	}

	if (!val_copy) {
		return false;
	}

	return yyjson_mut_arr_insert(handle->m_pVal_mut, val_copy, index);
}

bool JsonManager::ArrayInsertBool(JsonValue* handle, size_t index, bool value)
{
	return JsonTemplates::ArrayInsertTemplate<bool>(handle, index, value);
}

bool JsonManager::ArrayInsertInt(JsonValue* handle, size_t index, int value)
{
	return JsonTemplates::ArrayInsertTemplate<int>(handle, index, value);
}

bool JsonManager::ArrayInsertInt64(JsonValue* handle, size_t index, int64_t value)
{
	if (!handle || !handle->IsMutable()) {
		return false;
	}

	yyjson_mut_val* val = yyjson_mut_sint(handle->m_pDocument_mut->get(), value);

	if (!val) {
		return false;
	}

	return yyjson_mut_arr_insert(handle->m_pVal_mut, val, index);
}

bool JsonManager::ArrayInsertUint64(JsonValue* handle, size_t index, uint64_t value)
{
	if (!handle || !handle->IsMutable()) return false;
	yyjson_mut_val* val = yyjson_mut_uint(handle->m_pDocument_mut->get(), value);
	return val && yyjson_mut_arr_insert(handle->m_pVal_mut, val, index);
}

bool JsonManager::ArrayInsertDouble(JsonValue* handle, size_t index, double value)
{
	return JsonTemplates::ArrayInsertTemplate<double>(handle, index, value);
}

bool JsonManager::ArrayInsertString(JsonValue* handle, size_t index, const char* value)
{
	return JsonTemplates::ArrayInsertString(handle, index, value);
}

bool JsonManager::ArrayInsertNull(JsonValue* handle, size_t index)
{
	return JsonTemplates::ArrayInsertNull(handle, index);
}

bool JsonManager::ArrayPrepend(JsonValue* handle, JsonValue* value)
{
	if (!handle || !handle->IsMutable() || !value) {
		return false;
	}

	yyjson_mut_val* val_copy;
	if (value->IsMutable()) {
		val_copy = yyjson_mut_val_mut_copy(handle->m_pDocument_mut->get(), value->m_pVal_mut);
	} else {
		val_copy = yyjson_val_mut_copy(handle->m_pDocument_mut->get(), value->m_pVal);
	}

	if (!val_copy) {
		return false;
	}

	return yyjson_mut_arr_prepend(handle->m_pVal_mut, val_copy);
}

bool JsonManager::ArrayPrependBool(JsonValue* handle, bool value)
{
	return JsonTemplates::ArrayPrependTemplate<bool>(handle, value);
}

bool JsonManager::ArrayPrependInt(JsonValue* handle, int value)
{
	return JsonTemplates::ArrayPrependTemplate<int>(handle, value);
}

bool JsonManager::ArrayPrependInt64(JsonValue* handle, int64_t value)
{
	if (!handle || !handle->IsMutable()) {
		return false;
	}

	yyjson_mut_val* val = yyjson_mut_sint(handle->m_pDocument_mut->get(), value);

	if (!val) {
		return false;
	}

	return yyjson_mut_arr_prepend(handle->m_pVal_mut, val);
}

bool JsonManager::ArrayPrependUint64(JsonValue* handle, uint64_t value)
{
	if (!handle || !handle->IsMutable()) return false;
	yyjson_mut_val* val = yyjson_mut_uint(handle->m_pDocument_mut->get(), value);
	return val && yyjson_mut_arr_prepend(handle->m_pVal_mut, val);
}

bool JsonManager::ArrayPrependDouble(JsonValue* handle, double value)
{
	return JsonTemplates::ArrayPrependTemplate<double>(handle, value);
}

bool JsonManager::ArrayPrependString(JsonValue* handle, const char* value)
{
	return JsonTemplates::ArrayPrependString(handle, value);
}

bool JsonManager::ArrayPrependNull(JsonValue* handle)
{
	return JsonTemplates::ArrayPrependNull(handle);
}

bool JsonManager::ArrayRemove(JsonValue* handle, size_t index)
{
	if (!handle || !handle->IsMutable()) {
		return false;
	}

	size_t arr_size = yyjson_mut_arr_size(handle->m_pVal_mut);
	if (index >= arr_size) {
		return false;
	}

	return yyjson_mut_arr_remove(handle->m_pVal_mut, index) != nullptr;
}

bool JsonManager::ArrayRemoveFirst(JsonValue* handle)
{
	if (!handle || !handle->IsMutable()) {
		return false;
	}

	if (yyjson_mut_arr_size(handle->m_pVal_mut) == 0) {
		return false;
	}

	return yyjson_mut_arr_remove_first(handle->m_pVal_mut) != nullptr;
}

bool JsonManager::ArrayRemoveLast(JsonValue* handle)
{
	if (!handle || !handle->IsMutable()) {
		return false;
	}

	if (yyjson_mut_arr_size(handle->m_pVal_mut) == 0) {
		return false;
	}

	return yyjson_mut_arr_remove_last(handle->m_pVal_mut) != nullptr;
}

bool JsonManager::ArrayRemoveRange(JsonValue* handle, size_t start_index, size_t count)
{
	if (!handle || !handle->IsMutable()) {
		return false;
	}

	size_t arr_size = yyjson_mut_arr_size(handle->m_pVal_mut);

	if (start_index >= arr_size) {
		return false;
	}

	if (count == 0) {
		return true;
	}

	if (count > (arr_size - start_index)) {
		return false;
	}

	return yyjson_mut_arr_remove_range(handle->m_pVal_mut, start_index, count);
}

bool JsonManager::ArrayClear(JsonValue* handle)
{
	if (!handle || !handle->IsMutable()) {
		return false;
	}

	return yyjson_mut_arr_clear(handle->m_pVal_mut);
}

int JsonManager::ArrayIndexOfBool(JsonValue* handle, bool search_value)
{
	if (!handle) {
		return -1;
	}

	if (handle->IsMutable()) {
		size_t idx, max;
		yyjson_mut_val *val;
		yyjson_mut_arr_foreach(handle->m_pVal_mut, idx, max, val) {
			if (yyjson_mut_is_bool(val) && yyjson_mut_get_bool(val) == search_value) {
				return static_cast<int>(idx);
			}
		}
	} else {
		size_t idx, max;
		yyjson_val *val;
		yyjson_arr_foreach(handle->m_pVal, idx, max, val) {
			if (yyjson_is_bool(val) && yyjson_get_bool(val) == search_value) {
				return static_cast<int>(idx);
			}
		}
	}

	return -1;
}

int JsonManager::ArrayIndexOfString(JsonValue* handle, const char* search_value)
{
	if (!handle || !search_value) {
		return -1;
	}

	if (handle->IsMutable()) {
		size_t idx, max;
		yyjson_mut_val *val;
		yyjson_mut_arr_foreach(handle->m_pVal_mut, idx, max, val) {
			if (yyjson_mut_is_str(val) && strcmp(yyjson_mut_get_str(val), search_value) == 0) {
				return static_cast<int>(idx);
			}
		}
	} else {
		size_t idx, max;
		yyjson_val *val;
		yyjson_arr_foreach(handle->m_pVal, idx, max, val) {
			if (yyjson_is_str(val) && strcmp(yyjson_get_str(val), search_value) == 0) {
				return static_cast<int>(idx);
			}
		}
	}

	return -1;
}

int JsonManager::ArrayIndexOfInt(JsonValue* handle, int search_value)
{
	if (!handle) {
		return -1;
	}

	if (handle->IsMutable()) {
		size_t idx, max;
		yyjson_mut_val *val;
		yyjson_mut_arr_foreach(handle->m_pVal_mut, idx, max, val) {
			if (yyjson_mut_is_int(val) && yyjson_mut_get_int(val) == search_value) {
				return static_cast<int>(idx);
			}
		}
	} else {
		size_t idx, max;
		yyjson_val *val;
		yyjson_arr_foreach(handle->m_pVal, idx, max, val) {
			if (yyjson_is_int(val) && yyjson_get_int(val) == search_value) {
				return static_cast<int>(idx);
			}
		}
	}

	return -1;
}

int JsonManager::ArrayIndexOfInt64(JsonValue* handle, int64_t search_value)
{
	if (!handle) {
		return -1;
	}

	if (handle->IsMutable()) {
		size_t idx, max;
		yyjson_mut_val *val;
		yyjson_mut_arr_foreach(handle->m_pVal_mut, idx, max, val) {
			if (yyjson_mut_is_sint(val) && yyjson_mut_get_sint(val) == search_value) {
				return static_cast<int>(idx);
			}
		}
	} else {
		size_t idx, max;
		yyjson_val *val;
		yyjson_arr_foreach(handle->m_pVal, idx, max, val) {
			if (yyjson_is_sint(val) && yyjson_get_sint(val) == search_value) {
				return static_cast<int>(idx);
			}
		}
	}

	return -1;
}

int JsonManager::ArrayIndexOfUint64(JsonValue* handle, uint64_t search_value)
{
	if (!handle) return -1;
	if (handle->IsMutable()) {
		size_t idx, max; yyjson_mut_val* val;
		yyjson_mut_arr_foreach(handle->m_pVal_mut, idx, max, val) {
			if (yyjson_mut_is_uint(val) && yyjson_mut_get_uint(val) == search_value) return static_cast<int>(idx);
		}
	} else {
		size_t idx, max; yyjson_val* val;
		yyjson_arr_foreach(handle->m_pVal, idx, max, val) {
			if (yyjson_is_uint(val) && yyjson_get_uint(val) == search_value) return static_cast<int>(idx);
		}
	}
	return -1;
}

int JsonManager::ArrayIndexOfDouble(JsonValue* handle, double search_value)
{
	if (!handle) {
		return -1;
	}

	if (handle->IsMutable()) {
		size_t idx, max;
		yyjson_mut_val *val;
		yyjson_mut_arr_foreach(handle->m_pVal_mut, idx, max, val) {
			if (yyjson_mut_is_real(val)) {
				double val_num = yyjson_mut_get_real(val);
				if (EqualsFloatingPoint(val_num, search_value)) {
					return static_cast<int>(idx);
				}
			}
		}
	} else {
		size_t idx, max;
		yyjson_val *val;
		yyjson_arr_foreach(handle->m_pVal, idx, max, val) {
			if (yyjson_is_real(val)) {
				double val_num = yyjson_get_real(val);
				if (EqualsFloatingPoint(val_num, search_value)) {
					return static_cast<int>(idx);
				}
			}
		}
	}

	return -1;
}

bool JsonManager::ArraySort(JsonValue* handle, JSON_SORT_ORDER sort_mode)
{
	if (!handle || !handle->IsMutable()) {
		return false;
	}

	if (!yyjson_mut_is_arr(handle->m_pVal_mut)) {
		return false;
	}

	if (sort_mode < JSON_SORT_ASC || sort_mode > JSON_SORT_RANDOM) {
		return false;
	}

	size_t arr_size = yyjson_mut_arr_size(handle->m_pVal_mut);
	if (arr_size <= 1) return true;

	struct ValueInfo {
		yyjson_mut_val* val;
		uint8_t type;
		uint8_t subtype;  // 0=double, 1=signed int, 2=unsigned int
	};

	std::vector<ValueInfo> values;
	values.reserve(arr_size);

	size_t idx, max;
	yyjson_mut_val *val;
	yyjson_mut_arr_foreach(handle->m_pVal_mut, idx, max, val) {
		uint8_t type = yyjson_mut_get_type(val);
		uint8_t subtype = 0;

		if (type == YYJSON_TYPE_NUM) {
			if (yyjson_mut_is_int(val)) {
				subtype = yyjson_mut_is_sint(val) ? 1 : 2;
			}
		}

		values.push_back({val, type, subtype});
	}

	if (sort_mode == JSON_SORT_RANDOM) {
		std::shuffle(values.begin(), values.end(), m_randomGenerator);
	}
	else {
		auto compare = [sort_mode](const ValueInfo& a, const ValueInfo& b) {
			if (a.val == b.val) return false;

			if (a.type != b.type) {
				return sort_mode == JSON_SORT_ASC ? a.type < b.type : a.type > b.type;
			}

			switch (a.type) {
			case YYJSON_TYPE_STR: {
				const char* str_a = yyjson_mut_get_str(a.val);
				const char* str_b = yyjson_mut_get_str(b.val);
				int cmp = strcmp(str_a, str_b);
				return sort_mode == JSON_SORT_ASC ? cmp < 0 : cmp > 0;
			}
			case YYJSON_TYPE_NUM: {
				if (a.subtype > 0 && b.subtype > 0) {
					if (a.subtype == 1 && b.subtype == 1) {
						int64_t num_a = yyjson_mut_get_sint(a.val);
						int64_t num_b = yyjson_mut_get_sint(b.val);
						return sort_mode == JSON_SORT_ASC ? num_a < num_b : num_a > num_b;
					}
					else if (a.subtype == 2 && b.subtype == 2) {
						uint64_t num_a = yyjson_mut_get_uint(a.val);
						uint64_t num_b = yyjson_mut_get_uint(b.val);
						return sort_mode == JSON_SORT_ASC ? num_a < num_b : num_a > num_b;
					}
					else {
						int64_t signed_val;
						uint64_t unsigned_val;
						bool a_is_signed = (a.subtype == 1);

						if (a_is_signed) {
							signed_val = yyjson_mut_get_sint(a.val);
							unsigned_val = yyjson_mut_get_uint(b.val);

							if (signed_val < 0) {
								return sort_mode == JSON_SORT_ASC;
							}
							uint64_t a_as_unsigned = static_cast<uint64_t>(signed_val);
							return sort_mode == JSON_SORT_ASC ?
								a_as_unsigned < unsigned_val :
								a_as_unsigned > unsigned_val;
						} else {
							unsigned_val = yyjson_mut_get_uint(a.val);
							signed_val = yyjson_mut_get_sint(b.val);

							if (signed_val < 0) {
								return sort_mode == JSON_SORT_DESC;
							}
							uint64_t b_as_unsigned = static_cast<uint64_t>(signed_val);
							return sort_mode == JSON_SORT_ASC ?
								unsigned_val < b_as_unsigned :
								unsigned_val > b_as_unsigned;
						}
					}
				}
				double num_a = yyjson_mut_get_num(a.val);
				double num_b = yyjson_mut_get_num(b.val);
				return sort_mode == JSON_SORT_ASC ? num_a < num_b : num_a > num_b;
			}
			case YYJSON_TYPE_BOOL: {
				bool val_a = yyjson_mut_get_bool(a.val);
				bool val_b = yyjson_mut_get_bool(b.val);
				return sort_mode == JSON_SORT_ASC ? val_a < val_b : val_a > val_b;
			}
			default:
				return false;
			}
		};

		std::sort(values.begin(), values.end(), compare);
	}

	yyjson_mut_arr_clear(handle->m_pVal_mut);
	for (const auto& info : values) {
		yyjson_mut_arr_append(handle->m_pVal_mut, info.val);
	}

	return true;
}

bool JsonManager::ArrayRotate(JsonValue* handle, size_t idx)
{
	if (!handle || !handle->IsMutable()) {
		return false;
	}

	if (!yyjson_mut_is_arr(handle->m_pVal_mut)) {
		return false;
	}

	return yyjson_mut_arr_rotate(handle->m_pVal_mut, idx);
}

static const char* SkipPackSeparators(const char* ptr)
{
	while (*ptr && (std::isspace(static_cast<unsigned char>(*ptr)) || *ptr == ':' || *ptr == ',')) {
		ptr++;
	}
	return ptr;
}

static const char* SkipPackWhitespace(const char* ptr)
{
	while (*ptr && std::isspace(static_cast<unsigned char>(*ptr))) {
		ptr++;
	}
	return ptr;
}

class JsonPackParser
{
public:
	JsonPackParser(yyjson_mut_doc* doc, const char* format, IPackParamProvider* provider,
		char* error, size_t error_size)
		: doc_(doc), ptr_(format), provider_(provider), error_(error),
		error_size_(error_size) {}

	yyjson_mut_val* ParseRoot(const char** out_end_ptr)
	{
		if (!doc_ || !ptr_ || !*ptr_ || !provider_) {
			NativeErrorBuffer::Set(error_, error_size_, "Invalid argument(s)");
			return nullptr;
		}

		yyjson_mut_val* root = ParseContainer();
		if (!root) {
			return nullptr;
		}

		if (out_end_ptr) {
			*out_end_ptr = ptr_;
		}

		return root;
	}

private:
	yyjson_mut_val* ParseContainer()
	{
		switch (*ptr_) {
			case '{':
				return ParseObject();
			case '[':
				return ParseArray();
			default:
				NativeErrorBuffer::Set(error_, error_size_,
					"Invalid format string: expected '{' or '['");
				return nullptr;
		}
	}

	yyjson_mut_val* ParseObject()
	{
		yyjson_mut_val* root = yyjson_mut_obj(doc_);
		if (!root) {
			NativeErrorBuffer::Set(error_, error_size_, "Failed to create object");
			return nullptr;
		}

		ptr_ = SkipPackSeparators(ptr_ + 1);

		while (*ptr_ && *ptr_ != '}') {
			if (!ParseObjectMember(root)) {
				return nullptr;
			}
			ptr_ = SkipPackSeparators(ptr_);
		}

		if (*ptr_ != '}') {
			NativeErrorBuffer::Set(error_, error_size_, "Unexpected end of object format");
			return nullptr;
		}

		ptr_++;
		return root;
	}

	yyjson_mut_val* ParseArray()
	{
		yyjson_mut_val* root = yyjson_mut_arr(doc_);
		if (!root) {
			NativeErrorBuffer::Set(error_, error_size_, "Failed to create array");
			return nullptr;
		}

		ptr_ = SkipPackSeparators(ptr_ + 1);

		while (*ptr_ && *ptr_ != ']') {
			yyjson_mut_val* value = ParseValue();
			if (!value) {
				return nullptr;
			}

			if (!yyjson_mut_arr_append(root, value)) {
				NativeErrorBuffer::Set(error_, error_size_, "Failed to add value to array");
				return nullptr;
			}

			ptr_ = SkipPackSeparators(ptr_);
		}

		if (*ptr_ != ']') {
			NativeErrorBuffer::Set(error_, error_size_, "Unexpected end of array format");
			return nullptr;
		}

		ptr_++;
		return root;
	}

	bool ParseObjectMember(yyjson_mut_val* object)
	{
		if (*ptr_ != 's') {
			NativeErrorBuffer::Set(error_, error_size_,
				"Object key must be string, got '%c'", *ptr_);
			return false;
		}

		const char* key;
		if (!provider_->GetNextString(&key)) {
			NativeErrorBuffer::Set(error_, error_size_, "Invalid string key");
			return false;
		}

		yyjson_mut_val* key_val = yyjson_mut_strcpy(doc_, key);
		if (!key_val) {
			NativeErrorBuffer::Set(error_, error_size_, "Failed to create key");
			return false;
		}

		ptr_ = SkipPackSeparators(ptr_ + 1);
		yyjson_mut_val* value = ParseValue();
		if (!value) {
			return false;
		}

		if (!yyjson_mut_obj_add(object, key_val, value)) {
			NativeErrorBuffer::Set(error_, error_size_, "Failed to add value to object");
			return false;
		}

		return true;
	}

	yyjson_mut_val* ParseValue()
	{
		switch (*ptr_) {
			case 's':
				return ParseStringValue();
			case 'i':
				return ParseIntValue();
			case 'f':
				return ParseFloatValue();
			case 'b':
				return ParseBoolValue();
			case 'n':
				return ParseNullValue();
			case '{':
			case '[':
				return ParseContainer();
			case '\0':
				NativeErrorBuffer::Set(error_, error_size_, "Unexpected end of format string");
				return nullptr;
			default:
				NativeErrorBuffer::Set(error_, error_size_,
					"Invalid format character: %c", *ptr_);
				return nullptr;
		}
	}

	yyjson_mut_val* ParseStringValue()
	{
		const char* value;
		if (!provider_->GetNextString(&value)) {
			NativeErrorBuffer::Set(error_, error_size_, "Invalid string value");
			return nullptr;
		}

		yyjson_mut_val* val = yyjson_mut_strcpy(doc_, value);
		if (!val) {
			NativeErrorBuffer::Set(error_, error_size_, "Failed to create string value");
			return nullptr;
		}

		ptr_++;
		return val;
	}

	yyjson_mut_val* ParseIntValue()
	{
		int value;
		if (!provider_->GetNextInt(&value)) {
			NativeErrorBuffer::Set(error_, error_size_, "Invalid integer value");
			return nullptr;
		}

		yyjson_mut_val* val = yyjson_mut_int(doc_, value);
		if (!val) {
			NativeErrorBuffer::Set(error_, error_size_, "Failed to create integer value");
			return nullptr;
		}

		ptr_++;
		return val;
	}

	yyjson_mut_val* ParseFloatValue()
	{
		float value;
		if (!provider_->GetNextFloat(&value)) {
			NativeErrorBuffer::Set(error_, error_size_, "Invalid float value");
			return nullptr;
		}

		yyjson_mut_val* val = yyjson_mut_real(doc_, value);
		if (!val) {
			NativeErrorBuffer::Set(error_, error_size_, "Failed to create float value");
			return nullptr;
		}

		ptr_++;
		return val;
	}

	yyjson_mut_val* ParseBoolValue()
	{
		bool value;
		if (!provider_->GetNextBool(&value)) {
			NativeErrorBuffer::Set(error_, error_size_, "Invalid boolean value");
			return nullptr;
		}

		yyjson_mut_val* val = yyjson_mut_bool(doc_, value);
		if (!val) {
			NativeErrorBuffer::Set(error_, error_size_, "Failed to create boolean value");
			return nullptr;
		}

		ptr_++;
		return val;
	}

	yyjson_mut_val* ParseNullValue()
	{
		yyjson_mut_val* val = yyjson_mut_null(doc_);
		if (!val) {
			NativeErrorBuffer::Set(error_, error_size_, "Failed to create null value");
			return nullptr;
		}

		ptr_++;
		return val;
	}

	yyjson_mut_doc* doc_;
	const char* ptr_;
	IPackParamProvider* provider_;
	char* error_;
	size_t error_size_;
};

JsonValue* JsonManager::Pack(const char* format, IPackParamProvider* param_provider, char* error, size_t error_size)
{
	NativeErrorBuffer::Clear(error, error_size);

	if (!format || !param_provider) {
		NativeErrorBuffer::Set(error, error_size, "Invalid arguments");
		return nullptr;
	}

	auto pJSONValue = CreateWrapper();
	pJSONValue->m_pDocument_mut = CreateDocument();

	if (!pJSONValue->m_pDocument_mut) {
		NativeErrorBuffer::Set(error, error_size, "Failed to create document");
		return nullptr;
	}

	const char* end_ptr;
	JsonPackParser parser(pJSONValue->m_pDocument_mut->get(), format, param_provider,
		error, error_size);
	pJSONValue->m_pVal_mut = parser.ParseRoot(&end_ptr);

	if (!pJSONValue->m_pVal_mut) {
		return nullptr;
	}

	end_ptr = SkipPackWhitespace(end_ptr);
	if (*end_ptr) {
		NativeErrorBuffer::Set(error, error_size,
			"Unexpected trailing data in format string: %c", *end_ptr);
		return nullptr;
	}

	yyjson_mut_doc_set_root(pJSONValue->m_pDocument_mut->get(), pJSONValue->m_pVal_mut);

	return pJSONValue.release();
}

JsonValue* JsonManager::CreateBool(bool value)
{
	auto pJSONValue = CreateWrapper();
	pJSONValue->m_pDocument_mut = CreateDocument();

	if (!pJSONValue->m_pDocument_mut) {
		return nullptr;
	}

	pJSONValue->m_pVal_mut = yyjson_mut_bool(pJSONValue->m_pDocument_mut->get(), value);

	if (!pJSONValue->m_pVal_mut) {
		return nullptr;
	}

	yyjson_mut_doc_set_root(pJSONValue->m_pDocument_mut->get(), pJSONValue->m_pVal_mut);

	return pJSONValue.release();
}

JsonValue* JsonManager::CreateDouble(double value)
{
	auto pJSONValue = CreateWrapper();
	pJSONValue->m_pDocument_mut = CreateDocument();

	if (!pJSONValue->m_pDocument_mut) {
		return nullptr;
	}

	pJSONValue->m_pVal_mut = yyjson_mut_real(pJSONValue->m_pDocument_mut->get(), value);

	if (!pJSONValue->m_pVal_mut) {
		return nullptr;
	}

	yyjson_mut_doc_set_root(pJSONValue->m_pDocument_mut->get(), pJSONValue->m_pVal_mut);

	return pJSONValue.release();
}

JsonValue* JsonManager::CreateInt(int value)
{
	auto pJSONValue = CreateWrapper();
	pJSONValue->m_pDocument_mut = CreateDocument();

	if (!pJSONValue->m_pDocument_mut) {
		return nullptr;
	}

	pJSONValue->m_pVal_mut = yyjson_mut_int(pJSONValue->m_pDocument_mut->get(), value);

	if (!pJSONValue->m_pVal_mut) {
		return nullptr;
	}

	yyjson_mut_doc_set_root(pJSONValue->m_pDocument_mut->get(), pJSONValue->m_pVal_mut);

	return pJSONValue.release();
}

JsonValue* JsonManager::CreateInt64(int64_t value,
	char* error, size_t error_size)
{
	NativeErrorBuffer::Clear(error, error_size);

	auto pJSONValue = CreateWrapper();
	pJSONValue->m_pDocument_mut = CreateDocument();

	if (!pJSONValue->m_pDocument_mut) {
		NativeErrorBuffer::Set(error, error_size, "Failed to create document");
		return nullptr;
	}

	auto* doc = pJSONValue->m_pDocument_mut->get();

	pJSONValue->m_pVal_mut = yyjson_mut_sint(doc, value);

	if (!pJSONValue->m_pVal_mut) {
		NativeErrorBuffer::Set(error, error_size, "Failed to create integer64 value");
		return nullptr;
	}

	yyjson_mut_doc_set_root(pJSONValue->m_pDocument_mut->get(), pJSONValue->m_pVal_mut);

	return pJSONValue.release();
}

JsonValue* JsonManager::CreateUint64(uint64_t value, char* error, size_t error_size)
{
	NativeErrorBuffer::Clear(error, error_size);
	auto pJSONValue = CreateWrapper();
	pJSONValue->m_pDocument_mut = CreateDocument();
	if (!pJSONValue->m_pDocument_mut) {
		NativeErrorBuffer::Set(error, error_size, "Failed to create document");
		return nullptr;
	}
	pJSONValue->m_pVal_mut = yyjson_mut_uint(pJSONValue->m_pDocument_mut->get(), value);
	if (!pJSONValue->m_pVal_mut) {
		NativeErrorBuffer::Set(error, error_size, "Failed to create integer64 value");
		return nullptr;
	}
	yyjson_mut_doc_set_root(pJSONValue->m_pDocument_mut->get(), pJSONValue->m_pVal_mut);
	return pJSONValue.release();
}

JsonValue* JsonManager::CreateNull()
{
	auto pJSONValue = CreateWrapper();
	pJSONValue->m_pDocument_mut = CreateDocument();

	if (!pJSONValue->m_pDocument_mut) {
		return nullptr;
	}

	pJSONValue->m_pVal_mut = yyjson_mut_null(pJSONValue->m_pDocument_mut->get());

	if (!pJSONValue->m_pVal_mut) {
		return nullptr;
	}

	yyjson_mut_doc_set_root(pJSONValue->m_pDocument_mut->get(), pJSONValue->m_pVal_mut);

	return pJSONValue.release();
}

JsonValue* JsonManager::CreateString(const char* value)
{
	if (!value) {
		return nullptr;
	}

	auto pJSONValue = CreateWrapper();
	pJSONValue->m_pDocument_mut = CreateDocument();

	if (!pJSONValue->m_pDocument_mut) {
		return nullptr;
	}

	pJSONValue->m_pVal_mut = yyjson_mut_strcpy(pJSONValue->m_pDocument_mut->get(), value);

	if (!pJSONValue->m_pVal_mut) {
		return nullptr;
	}

	yyjson_mut_doc_set_root(pJSONValue->m_pDocument_mut->get(), pJSONValue->m_pVal_mut);

	return pJSONValue.release();
}

bool JsonManager::GetBool(JsonValue* handle, bool* out_value)
{
	if (!handle || !out_value) {
		return false;
	}

	if (handle->IsMutable()) {
		if (!yyjson_mut_is_bool(handle->m_pVal_mut)) {
			return false;
		}
		*out_value = yyjson_mut_get_bool(handle->m_pVal_mut);
		return true;
	} else {
		if (!yyjson_is_bool(handle->m_pVal)) {
			return false;
		}
		*out_value = yyjson_get_bool(handle->m_pVal);
		return true;
	}
}

bool JsonManager::GetDouble(JsonValue* handle, double* out_value)
{
	if (!handle || !out_value) {
		return false;
	}

	if (handle->IsMutable()) {
		if (!yyjson_mut_is_num(handle->m_pVal_mut)) {
			return false;
		}
		*out_value = yyjson_mut_get_num(handle->m_pVal_mut);
		return true;
	} else {
		if (!yyjson_is_num(handle->m_pVal)) {
			return false;
		}
		*out_value = yyjson_get_num(handle->m_pVal);
		return true;
	}
}

bool JsonManager::GetInt(JsonValue* handle, int* out_value)
{
	if (!handle || !out_value) {
		return false;
	}

	if (handle->IsMutable()) {
		if (!yyjson_mut_is_int(handle->m_pVal_mut)) {
			return false;
		}
		*out_value = yyjson_mut_get_int(handle->m_pVal_mut);
		return true;
	} else {
		if (!yyjson_is_int(handle->m_pVal)) {
			return false;
		}
		*out_value = yyjson_get_int(handle->m_pVal);
		return true;
	}
}

bool JsonManager::GetInt64(JsonValue* handle, int64_t* out_value)
{
	if (!handle || !out_value) {
		return false;
	}

	if (handle->IsMutable()) {
		return ReadInt64FromMutVal(handle->m_pVal_mut, out_value);
	} else {
		return ReadInt64FromVal(handle->m_pVal, out_value);
	}
}

bool JsonManager::GetUint64(JsonValue* handle, uint64_t* out_value)
{
	if (!handle || !out_value) return false;
	return handle->IsMutable()
		? ReadUint64FromMutVal(handle->m_pVal_mut, out_value)
		: ReadUint64FromVal(handle->m_pVal, out_value);
}

bool JsonManager::GetString(JsonValue* handle, const char** out_str, size_t* out_len)
{
	if (!handle || !out_str) {
		return false;
	}

	if (handle->IsMutable()) {
		if (!yyjson_mut_is_str(handle->m_pVal_mut)) {
			return false;
		}
		*out_str = yyjson_mut_get_str(handle->m_pVal_mut);
		if (out_len) {
			*out_len = yyjson_mut_get_len(handle->m_pVal_mut);
		}
		return true;
	} else {
		if (!yyjson_is_str(handle->m_pVal)) {
			return false;
		}
		*out_str = yyjson_get_str(handle->m_pVal);
		if (out_len) {
			*out_len = yyjson_get_len(handle->m_pVal);
		}
		return true;
	}
}

JsonValue* JsonManager::PtrGet(JsonValue* handle, const char* path, char* error, size_t error_size)
{
	NativeErrorBuffer::Clear(error, error_size);

	auto pJSONValue = CreateWrapper();
	PtrResolvedValue resolved;
	if (!ResolvePtrValue(handle, path, &resolved, error, error_size)) {
		return nullptr;
	}

	if (resolved.is_mutable) {
		pJSONValue->m_pDocument_mut = handle->m_pDocument_mut;
		pJSONValue->m_pVal_mut = resolved.mut;
	} else {
		pJSONValue->m_pDocument = handle->m_pDocument;
		pJSONValue->m_pVal = resolved.imm;
	}

	return pJSONValue.release();
}

bool JsonManager::PtrGetBool(JsonValue* handle, const char* path, bool* out_value, char* error, size_t error_size)
{
	NativeErrorBuffer::Clear(error, error_size);

	if (!out_value) {
		NativeErrorBuffer::Set(error, error_size, "Invalid parameters");
		return false;
	}

	PtrResolvedValue resolved;
	if (!ResolvePtrValue(handle, path, &resolved, error, error_size)) {
		return false;
	}

	if (resolved.is_mutable) {
		if (!yyjson_mut_is_bool(resolved.mut)) {
			return ReportPtrTypeMismatch(resolved, path, "boolean value", error, error_size);
		}

		*out_value = yyjson_mut_get_bool(resolved.mut);
	} else {
		if (!yyjson_is_bool(resolved.imm)) {
			return ReportPtrTypeMismatch(resolved, path, "boolean value", error, error_size);
		}

		*out_value = yyjson_get_bool(resolved.imm);
	}

	return true;
}

bool JsonManager::PtrGetDouble(JsonValue* handle, const char* path, double* out_value, char* error, size_t error_size)
{
	NativeErrorBuffer::Clear(error, error_size);

	if (!out_value) {
		NativeErrorBuffer::Set(error, error_size, "Invalid parameters");
		return false;
	}

	PtrResolvedValue resolved;
	if (!ResolvePtrValue(handle, path, &resolved, error, error_size)) {
		return false;
	}

	if (resolved.is_mutable) {
		if (!yyjson_mut_is_num(resolved.mut)) {
			return ReportPtrTypeMismatch(resolved, path, "number value", error, error_size);
		}

		*out_value = yyjson_mut_get_num(resolved.mut);
	} else {
		if (!yyjson_is_num(resolved.imm)) {
			return ReportPtrTypeMismatch(resolved, path, "number value", error, error_size);
		}

		*out_value = yyjson_get_num(resolved.imm);
	}

	return true;
}

bool JsonManager::PtrGetInt(JsonValue* handle, const char* path, int* out_value, char* error, size_t error_size)
{
	NativeErrorBuffer::Clear(error, error_size);

	if (!out_value) {
		NativeErrorBuffer::Set(error, error_size, "Invalid parameters");
		return false;
	}

	PtrResolvedValue resolved;
	if (!ResolvePtrValue(handle, path, &resolved, error, error_size)) {
		return false;
	}

	if (resolved.is_mutable) {
		if (!yyjson_mut_is_int(resolved.mut)) {
			return ReportPtrTypeMismatch(resolved, path, "integer value", error, error_size);
		}

		*out_value = yyjson_mut_get_int(resolved.mut);
	} else {
		if (!yyjson_is_int(resolved.imm)) {
			return ReportPtrTypeMismatch(resolved, path, "integer value", error, error_size);
		}

		*out_value = yyjson_get_int(resolved.imm);
	}

	return true;
}

bool JsonManager::PtrGetInt64(JsonValue* handle, const char* path, int64_t* out_value, char* error, size_t error_size)
{
	NativeErrorBuffer::Clear(error, error_size);

	if (!out_value) {
		NativeErrorBuffer::Set(error, error_size, "Invalid parameters");
		return false;
	}

	PtrResolvedValue resolved;
	if (!ResolvePtrValue(handle, path, &resolved, error, error_size)) {
		return false;
	}

	if (resolved.is_mutable) {
		if (!ReadInt64FromMutVal(resolved.mut, out_value)) return ReportPtrTypeMismatch(resolved, path, "int64 value", error, error_size);
	} else {
		if (!ReadInt64FromVal(resolved.imm, out_value)) return ReportPtrTypeMismatch(resolved, path, "int64 value", error, error_size);
	}

	return true;
}

bool JsonManager::PtrGetUint64(JsonValue* handle, const char* path, uint64_t* out_value, char* error, size_t error_size)
{
	NativeErrorBuffer::Clear(error, error_size);
	if (!out_value) { NativeErrorBuffer::Set(error, error_size, "Invalid parameters"); return false; }
	PtrResolvedValue resolved;
	if (!ResolvePtrValue(handle, path, &resolved, error, error_size)) return false;
	bool ok = resolved.is_mutable ? ReadUint64FromMutVal(resolved.mut, out_value) : ReadUint64FromVal(resolved.imm, out_value);
	return ok || ReportPtrTypeMismatch(resolved, path, "uint64 value", error, error_size);
}

bool JsonManager::PtrGetString(JsonValue* handle, const char* path, const char** out_str, size_t* out_len, char* error, size_t error_size)
{
	NativeErrorBuffer::Clear(error, error_size);

	if (!out_str) {
		NativeErrorBuffer::Set(error, error_size, "Invalid parameters");
		return false;
	}

	PtrResolvedValue resolved;
	if (!ResolvePtrValue(handle, path, &resolved, error, error_size)) {
		return false;
	}

	if (resolved.is_mutable) {
		if (!yyjson_mut_is_str(resolved.mut)) {
			return ReportPtrTypeMismatch(resolved, path, "string value", error, error_size);
		}

		*out_str = yyjson_mut_get_str(resolved.mut);
		if (out_len) {
			*out_len = yyjson_mut_get_len(resolved.mut);
		}
	} else {
		if (!yyjson_is_str(resolved.imm)) {
			return ReportPtrTypeMismatch(resolved, path, "string value", error, error_size);
		}

		*out_str = yyjson_get_str(resolved.imm);
		if (out_len) {
			*out_len = yyjson_get_len(resolved.imm);
		}
	}

	return true;
}

bool JsonManager::PtrGetIsNull(JsonValue* handle, const char* path, bool* out_is_null, char* error, size_t error_size)
{
	NativeErrorBuffer::Clear(error, error_size);

	if (!out_is_null) {
		NativeErrorBuffer::Set(error, error_size, "Invalid parameters");
		return false;
	}

	PtrResolvedValue resolved;
	if (!ResolvePtrValue(handle, path, &resolved, error, error_size)) {
		return false;
	}

	if (resolved.is_mutable) {
		*out_is_null = yyjson_mut_is_null(resolved.mut);
	} else {
		*out_is_null = yyjson_is_null(resolved.imm);
	}

	return true;
}

bool JsonManager::PtrGetLength(JsonValue* handle, const char* path, size_t* out_len, char* error, size_t error_size)
{
	NativeErrorBuffer::Clear(error, error_size);

	if (!out_len) {
		NativeErrorBuffer::Set(error, error_size, "Invalid parameters");
		return false;
	}

	PtrResolvedValue resolved;
	if (!ResolvePtrValue(handle, path, &resolved, error, error_size)) {
		return false;
	}

	if (resolved.is_mutable) {
		if (yyjson_mut_is_str(resolved.mut)) {
			*out_len = yyjson_mut_get_len(resolved.mut) + 1;
		} else {
			*out_len = yyjson_mut_get_len(resolved.mut);
		}
	} else {
		if (yyjson_is_str(resolved.imm)) {
			*out_len = yyjson_get_len(resolved.imm) + 1;
		} else {
			*out_len = yyjson_get_len(resolved.imm);
		}
	}

	return true;
}

bool JsonManager::PtrSet(JsonValue* handle, const char* path, JsonValue* value, char* error, size_t error_size)
{
	NativeErrorBuffer::Clear(error, error_size);

	if (!ValidateMutablePtrParams(handle, path, error, error_size)) {
		return false;
	}

	if (!value) {
		NativeErrorBuffer::Set(error, error_size, "Invalid parameters or immutable document");
		return false;
	}

	yyjson_mut_val* val_copy;
	if (value->IsMutable()) {
		val_copy = yyjson_mut_val_mut_copy(handle->m_pDocument_mut->get(), value->m_pVal_mut);
	} else {
		val_copy = yyjson_val_mut_copy(handle->m_pDocument_mut->get(), value->m_pVal);
	}

	if (!val_copy) {
		NativeErrorBuffer::Set(error, error_size, "Failed to copy JSON value");
		return false;
	}

	return ApplyPtrMutation(handle, path, val_copy, PtrMutationOp::Set, error, error_size,
		"Failed to copy JSON value");
}

bool JsonManager::PtrSetBool(JsonValue* handle, const char* path, bool value, char* error, size_t error_size)
{
	NativeErrorBuffer::Clear(error, error_size);

	if (!ValidateMutablePtrParams(handle, path, error, error_size)) {
		return false;
	}

	yyjson_mut_val* val = yyjson_mut_bool(handle->m_pDocument_mut->get(), value);
	return ApplyPtrMutation(handle, path, val, PtrMutationOp::Set, error, error_size,
		"Failed to create JSON value");
}

bool JsonManager::PtrSetDouble(JsonValue* handle, const char* path, double value, char* error, size_t error_size)
{
	NativeErrorBuffer::Clear(error, error_size);

	if (!ValidateMutablePtrParams(handle, path, error, error_size)) {
		return false;
	}

	yyjson_mut_val* val = yyjson_mut_real(handle->m_pDocument_mut->get(), value);
	return ApplyPtrMutation(handle, path, val, PtrMutationOp::Set, error, error_size,
		"Failed to create JSON value");
}

bool JsonManager::PtrSetInt(JsonValue* handle, const char* path, int value, char* error, size_t error_size)
{
	NativeErrorBuffer::Clear(error, error_size);

	if (!ValidateMutablePtrParams(handle, path, error, error_size)) {
		return false;
	}

	yyjson_mut_val* val = yyjson_mut_int(handle->m_pDocument_mut->get(), value);
	return ApplyPtrMutation(handle, path, val, PtrMutationOp::Set, error, error_size,
		"Failed to create JSON value");
}

bool JsonManager::PtrSetInt64(JsonValue* handle, const char* path, int64_t value, char* error, size_t error_size)
{
	NativeErrorBuffer::Clear(error, error_size);

	if (!ValidateMutablePtrParams(handle, path, error, error_size)) {
		return false;
	}

	yyjson_mut_val* val = yyjson_mut_sint(handle->m_pDocument_mut->get(), value);
	return ApplyPtrMutation(handle, path, val, PtrMutationOp::Set, error, error_size,
		"Failed to create JSON value");
}

bool JsonManager::PtrSetUint64(JsonValue* handle, const char* path, uint64_t value, char* error, size_t error_size)
{
	NativeErrorBuffer::Clear(error, error_size);
	if (!ValidateMutablePtrParams(handle, path, error, error_size)) return false;
	return ApplyPtrMutation(handle, path, yyjson_mut_uint(handle->m_pDocument_mut->get(), value), PtrMutationOp::Set, error, error_size, "Failed to create JSON value");
}

bool JsonManager::PtrSetString(JsonValue* handle, const char* path, const char* value, char* error, size_t error_size)
{
	NativeErrorBuffer::Clear(error, error_size);

	if (!ValidateMutablePtrParams(handle, path, error, error_size)) {
		return false;
	}

	if (!value) {
		NativeErrorBuffer::Set(error, error_size, "Invalid parameters or immutable document");
		return false;
	}

	yyjson_mut_val* val = yyjson_mut_strcpy(handle->m_pDocument_mut->get(), value);
	return ApplyPtrMutation(handle, path, val, PtrMutationOp::Set, error, error_size,
		"Failed to create JSON value");
}

bool JsonManager::PtrSetNull(JsonValue* handle, const char* path, char* error, size_t error_size)
{
	NativeErrorBuffer::Clear(error, error_size);

	if (!ValidateMutablePtrParams(handle, path, error, error_size)) {
		return false;
	}

	yyjson_mut_val* val = yyjson_mut_null(handle->m_pDocument_mut->get());
	return ApplyPtrMutation(handle, path, val, PtrMutationOp::Set, error, error_size,
		"Failed to create JSON value");
}

bool JsonManager::PtrAdd(JsonValue* handle, const char* path, JsonValue* value, char* error, size_t error_size)
{
	NativeErrorBuffer::Clear(error, error_size);

	if (!ValidateMutablePtrParams(handle, path, error, error_size)) {
		return false;
	}

	if (!value) {
		NativeErrorBuffer::Set(error, error_size, "Invalid parameters or immutable document");
		return false;
	}

	yyjson_mut_val* val_copy;
	if (value->IsMutable()) {
		val_copy = yyjson_mut_val_mut_copy(handle->m_pDocument_mut->get(), value->m_pVal_mut);
	} else {
		val_copy = yyjson_val_mut_copy(handle->m_pDocument_mut->get(), value->m_pVal);
	}

	if (!val_copy) {
		NativeErrorBuffer::Set(error, error_size, "Failed to copy JSON value");
		return false;
	}

	return ApplyPtrMutation(handle, path, val_copy, PtrMutationOp::Add, error, error_size,
		"Failed to copy JSON value");
}

bool JsonManager::PtrAddBool(JsonValue* handle, const char* path, bool value, char* error, size_t error_size)
{
	NativeErrorBuffer::Clear(error, error_size);

	if (!ValidateMutablePtrParams(handle, path, error, error_size)) {
		return false;
	}

	yyjson_mut_val* val = yyjson_mut_bool(handle->m_pDocument_mut->get(), value);
	return ApplyPtrMutation(handle, path, val, PtrMutationOp::Add, error, error_size,
		"Failed to create JSON value");
}

bool JsonManager::PtrAddDouble(JsonValue* handle, const char* path, double value, char* error, size_t error_size)
{
	NativeErrorBuffer::Clear(error, error_size);

	if (!ValidateMutablePtrParams(handle, path, error, error_size)) {
		return false;
	}

	yyjson_mut_val* val = yyjson_mut_real(handle->m_pDocument_mut->get(), value);
	return ApplyPtrMutation(handle, path, val, PtrMutationOp::Add, error, error_size,
		"Failed to create JSON value");
}

bool JsonManager::PtrAddInt(JsonValue* handle, const char* path, int value, char* error, size_t error_size)
{
	NativeErrorBuffer::Clear(error, error_size);

	if (!ValidateMutablePtrParams(handle, path, error, error_size)) {
		return false;
	}

	yyjson_mut_val* val = yyjson_mut_int(handle->m_pDocument_mut->get(), value);
	return ApplyPtrMutation(handle, path, val, PtrMutationOp::Add, error, error_size,
		"Failed to create JSON value");
}

bool JsonManager::PtrAddInt64(JsonValue* handle, const char* path, int64_t value, char* error, size_t error_size)
{
	NativeErrorBuffer::Clear(error, error_size);

	if (!ValidateMutablePtrParams(handle, path, error, error_size)) {
		return false;
	}

	yyjson_mut_val* val = yyjson_mut_sint(handle->m_pDocument_mut->get(), value);
	return ApplyPtrMutation(handle, path, val, PtrMutationOp::Add, error, error_size,
		"Failed to create JSON value");
}

bool JsonManager::PtrAddUint64(JsonValue* handle, const char* path, uint64_t value, char* error, size_t error_size)
{
	NativeErrorBuffer::Clear(error, error_size);
	if (!ValidateMutablePtrParams(handle, path, error, error_size)) return false;
	return ApplyPtrMutation(handle, path, yyjson_mut_uint(handle->m_pDocument_mut->get(), value), PtrMutationOp::Add, error, error_size, "Failed to create JSON value");
}

bool JsonManager::PtrAddString(JsonValue* handle, const char* path, const char* value, char* error, size_t error_size)
{
	NativeErrorBuffer::Clear(error, error_size);

	if (!ValidateMutablePtrParams(handle, path, error, error_size)) {
		return false;
	}

	if (!value) {
		NativeErrorBuffer::Set(error, error_size, "Invalid parameters or immutable document");
		return false;
	}

	yyjson_mut_val* val = yyjson_mut_strcpy(handle->m_pDocument_mut->get(), value);
	return ApplyPtrMutation(handle, path, val, PtrMutationOp::Add, error, error_size,
		"Failed to create JSON value");
}

bool JsonManager::PtrAddNull(JsonValue* handle, const char* path, char* error, size_t error_size)
{
	NativeErrorBuffer::Clear(error, error_size);

	if (!ValidateMutablePtrParams(handle, path, error, error_size)) {
		return false;
	}

	yyjson_mut_val* val = yyjson_mut_null(handle->m_pDocument_mut->get());
	return ApplyPtrMutation(handle, path, val, PtrMutationOp::Add, error, error_size,
		"Failed to create JSON value");
}

bool JsonManager::PtrRemove(JsonValue* handle, const char* path, char* error, size_t error_size)
{
	NativeErrorBuffer::Clear(error, error_size);

	if (!ValidateMutablePtrParams(handle, path, error, error_size)) {
		return false;
	}

	size_t path_len = strlen(path);
	yyjson_ptr_err ptrRemoveError{};
	bool success = yyjson_mut_doc_ptr_removex(handle->m_pDocument_mut->get(), path, path_len, nullptr, &ptrRemoveError) != nullptr;

	if (!success && ptrRemoveError.code) {
		SetPtrOperationError("remove", ptrRemoveError, path, error, error_size);
	}

	return success;
}

JsonManager::PtrGetValueResult JsonManager::PtrGetValueInternal(JsonValue* handle, const char* path)
{
	PtrGetValueResult result;
	result.success = false;

	PtrResolvedValue resolved;
	if (!ResolvePtrValue(handle, path, &resolved, nullptr, 0)) {
		return result;
	}

	result.success = true;
	if (resolved.is_mutable) {
		result.mut_val = resolved.mut;
	} else {
		result.imm_val = resolved.imm;
	}

	return result;
}

JsonValue* JsonManager::PtrTryGet(JsonValue* handle, const char* path)
{
	if (!handle || !path) {
		return nullptr;
	}

	auto result = PtrGetValueInternal(handle, path);
	if (!result.success) {
		return nullptr;
	}

	auto pJSONValue = CreateWrapper();
	if (handle->IsMutable()) {
		pJSONValue->m_pDocument_mut = handle->m_pDocument_mut;
		pJSONValue->m_pVal_mut = result.mut_val;
	} else {
		pJSONValue->m_pDocument = handle->m_pDocument;
		pJSONValue->m_pVal = result.imm_val;
	}

	return pJSONValue.release();
}

bool JsonManager::PtrTryGetBool(JsonValue* handle, const char* path, bool* out_value)
{
	if (!handle || !path || !out_value) {
		return false;
	}

	auto result = PtrGetValueInternal(handle, path);
	if (!result.success) {
		return false;
	}

	if (handle->IsMutable()) {
		if (!yyjson_mut_is_bool(result.mut_val)) {
			return false;
		}
		*out_value = yyjson_mut_get_bool(result.mut_val);
		return true;
	} else {
		if (!yyjson_is_bool(result.imm_val)) {
			return false;
		}
		*out_value = yyjson_get_bool(result.imm_val);
		return true;
	}
}

bool JsonManager::PtrTryGetDouble(JsonValue* handle, const char* path, double* out_value)
{
	if (!handle || !path || !out_value) {
		return false;
	}

	auto result = PtrGetValueInternal(handle, path);
	if (!result.success) {
		return false;
	}

	if (handle->IsMutable()) {
		if (!yyjson_mut_is_num(result.mut_val)) {
			return false;
		}
		*out_value = yyjson_mut_get_num(result.mut_val);
		return true;
	} else {
		if (!yyjson_is_num(result.imm_val)) {
			return false;
		}
		*out_value = yyjson_get_num(result.imm_val);
		return true;
	}
}

bool JsonManager::PtrTryGetInt(JsonValue* handle, const char* path, int* out_value)
{
	if (!handle || !path || !out_value) {
		return false;
	}

	auto result = PtrGetValueInternal(handle, path);
	if (!result.success) {
		return false;
	}

	if (handle->IsMutable()) {
		if (!yyjson_mut_is_int(result.mut_val)) {
			return false;
		}
		*out_value = yyjson_mut_get_int(result.mut_val);
		return true;
	} else {
		if (!yyjson_is_int(result.imm_val)) {
			return false;
		}
		*out_value = yyjson_get_int(result.imm_val);
		return true;
	}
}

bool JsonManager::PtrTryGetInt64(JsonValue* handle, const char* path, int64_t* out_value)
{
	if (!handle || !path || !out_value) {
		return false;
	}

	auto result = PtrGetValueInternal(handle, path);
	if (!result.success) {
		return false;
	}

	return handle->IsMutable() ? ReadInt64FromMutVal(result.mut_val, out_value) : ReadInt64FromVal(result.imm_val, out_value);
}

bool JsonManager::PtrTryGetUint64(JsonValue* handle, const char* path, uint64_t* out_value)
{
	if (!handle || !path || !out_value) return false;
	auto result = PtrGetValueInternal(handle, path);
	if (!result.success) return false;
	return handle->IsMutable() ? ReadUint64FromMutVal(result.mut_val, out_value) : ReadUint64FromVal(result.imm_val, out_value);
}

bool JsonManager::PtrTryGetString(JsonValue* handle, const char* path, const char** out_str, size_t* out_len)
{
	if (!handle || !path || !out_str) {
		return false;
	}

	auto result = PtrGetValueInternal(handle, path);
	if (!result.success) {
		return false;
	}

	if (handle->IsMutable()) {
		if (!yyjson_mut_is_str(result.mut_val)) {
			return false;
		}
		*out_str = yyjson_mut_get_str(result.mut_val);
		if (out_len) {
			*out_len = yyjson_mut_get_len(result.mut_val);
		}
		return true;
	} else {
		if (!yyjson_is_str(result.imm_val)) {
			return false;
		}
		*out_str = yyjson_get_str(result.imm_val);
		if (out_len) {
			*out_len = yyjson_get_len(result.imm_val);
		}
		return true;
	}
}

void JsonManager::ReleaseJsonValue(JsonValue* value)
{
	if (value) {
		delete value;
	}
}

HandleType_t JsonManager::GetJsonHandleType()
{
	return g_JsonType;
}

JsonValue* JsonManager::GetValueFromHandle(IPluginContext* pContext, Handle_t handle)
{
	HandleError err;
	HandleSecurity sec(pContext->GetIdentity(), myself->GetIdentity());

	JsonValue* pJSONValue;
	if ((err = handlesys->ReadHandle(handle, g_JsonType, &sec, (void**)&pJSONValue)) != HandleError_None)
	{
		pContext->ReportError("Invalid JSON handle %x (error %d)", handle, err);
		return nullptr;
	}

	return pJSONValue;
}

JsonArrIter* JsonManager::ArrIterInit(JsonValue* handle)
{
	return ArrIterWith(handle);
}

JsonArrIter* JsonManager::ArrIterWith(JsonValue* handle)
{
	if (!handle || !IsArray(handle)) {
		return nullptr;
	}

	auto iter = new JsonArrIter();
	iter->m_isMutable = handle->IsMutable();
	iter->m_pDocument_mut = handle->m_pDocument_mut;
	iter->m_pDocument = handle->m_pDocument;
	iter->m_rootMut = nullptr;
	iter->m_rootImm = nullptr;

	if (handle->IsMutable()) {
		iter->m_rootMut = handle->m_pVal_mut;
		if (!iter->m_rootMut || !yyjson_mut_arr_iter_init(iter->m_rootMut, &iter->m_iterMut)) {
			delete iter;
			return nullptr;
		}
	} else {
		iter->m_rootImm = handle->m_pVal;
		if (!iter->m_rootImm || !yyjson_arr_iter_init(iter->m_rootImm, &iter->m_iterImm)) {
			delete iter;
			return nullptr;
		}
	}

	iter->m_initialized = true;
	return iter;
}

bool JsonManager::ArrIterReset(JsonArrIter* iter)
{
	if (!iter || !iter->m_initialized) {
		return false;
	}

	bool success;
	if (iter->m_isMutable) {
		success = iter->m_rootMut && yyjson_mut_arr_iter_init(iter->m_rootMut, &iter->m_iterMut);
	} else {
		success = iter->m_rootImm && yyjson_arr_iter_init(iter->m_rootImm, &iter->m_iterImm);
	}

	if (!success) {
		iter->m_initialized = false;
		return false;
	}

	return true;
}

JsonValue* JsonManager::ArrIterNext(JsonArrIter* iter)
{
	if (!iter || !iter->m_initialized) {
		return nullptr;
	}

	JsonValue* val;

	if (iter->m_isMutable) {
		yyjson_mut_val* raw_val = yyjson_mut_arr_iter_next(&iter->m_iterMut);
		if (!raw_val) {
			return nullptr;
		}

		auto pWrapper = CreateWrapper();
		pWrapper->m_pDocument_mut = iter->m_pDocument_mut;
		pWrapper->m_pVal_mut = raw_val;
		val = pWrapper.release();
	} else {
		yyjson_val* raw_val = yyjson_arr_iter_next(&iter->m_iterImm);
		if (!raw_val) {
			return nullptr;
		}

		auto pWrapper = CreateWrapper();
		pWrapper->m_pDocument = iter->m_pDocument;
		pWrapper->m_pVal = raw_val;
		val = pWrapper.release();
	}

	return val;
}

bool JsonManager::ArrIterHasNext(JsonArrIter* iter)
{
	if (!iter || !iter->m_initialized) {
		return false;
	}

	if (iter->m_isMutable) {
		return yyjson_mut_arr_iter_has_next(&iter->m_iterMut);
	} else {
		return yyjson_arr_iter_has_next(&iter->m_iterImm);
	}
}

size_t JsonManager::ArrIterGetIndex(JsonArrIter* iter)
{
	if (!iter || !iter->m_initialized) {
		return SIZE_MAX;
	}

	if (iter->m_isMutable) {
		if (iter->m_iterMut.idx == 0) {
			return SIZE_MAX;
		}
		return iter->m_iterMut.idx - 1;
	} else {
		if (iter->m_iterImm.idx == 0) {
			return SIZE_MAX;
		}
		return iter->m_iterImm.idx - 1;
	}
}

bool JsonManager::ArrIterRemove(JsonArrIter* iter)
{
	if (!iter || !iter->m_isMutable) {
		return false;
	}

	return yyjson_mut_arr_iter_remove(&iter->m_iterMut) != nullptr;
}

JsonObjIter* JsonManager::ObjIterInit(JsonValue* handle)
{
	return ObjIterWith(handle);
}

JsonObjIter* JsonManager::ObjIterWith(JsonValue* handle)
{
	if (!handle || !IsObject(handle)) {
		return nullptr;
	}

	auto iter = new JsonObjIter();
	iter->m_isMutable = handle->IsMutable();
	iter->m_pDocument_mut = handle->m_pDocument_mut;
	iter->m_pDocument = handle->m_pDocument;
	iter->m_rootMut = nullptr;
	iter->m_rootImm = nullptr;

	if (handle->IsMutable()) {
		iter->m_rootMut = handle->m_pVal_mut;
		if (!iter->m_rootMut || !yyjson_mut_obj_iter_init(iter->m_rootMut, &iter->m_iterMut)) {
			delete iter;
			return nullptr;
		}
	} else {
		iter->m_rootImm = handle->m_pVal;
		if (!iter->m_rootImm || !yyjson_obj_iter_init(iter->m_rootImm, &iter->m_iterImm)) {
			delete iter;
			return nullptr;
		}
	}

	iter->m_initialized = true;
	return iter;
}

bool JsonManager::ObjIterReset(JsonObjIter* iter)
{
	if (!iter || !iter->m_initialized) {
		return false;
	}

	bool success;
	if (iter->m_isMutable) {
		success = iter->m_rootMut && yyjson_mut_obj_iter_init(iter->m_rootMut, &iter->m_iterMut);
	} else {
		success = iter->m_rootImm && yyjson_obj_iter_init(iter->m_rootImm, &iter->m_iterImm);
	}

	if (!success) {
		iter->m_initialized = false;
		return false;
	}

	iter->m_currentKey = nullptr;
	return true;
}

bool JsonManager::ObjIterNext(JsonObjIter* iter, const char** out_key, size_t* out_len)
{
	if (!iter || !iter->m_initialized || !out_key) {
		return false;
	}

	if (iter->m_isMutable) {
		yyjson_mut_val* current_key = yyjson_mut_obj_iter_next(&iter->m_iterMut);
		if (!current_key) {
			return false;
		}
		iter->m_currentKey = current_key;
		*out_key = yyjson_mut_get_str(current_key);
		if (out_len) *out_len = yyjson_mut_get_len(current_key);
		return *out_key != nullptr;
	} else {
		yyjson_val* key = yyjson_obj_iter_next(&iter->m_iterImm);
		if (!key) return false;
		iter->m_currentKey = key;
		*out_key = yyjson_get_str(key);
		if (out_len) *out_len = yyjson_get_len(key);
		return *out_key != nullptr;
	}
}

bool JsonManager::ObjIterHasNext(JsonObjIter* iter)
{
	if (!iter || !iter->m_initialized) {
		return false;
	}

	if (iter->m_isMutable) {
		return yyjson_mut_obj_iter_has_next(&iter->m_iterMut);
	} else {
		return yyjson_obj_iter_has_next(&iter->m_iterImm);
	}
}

JsonValue* JsonManager::ObjIterGetVal(JsonObjIter* iter)
{
	if (!iter || !iter->m_initialized || !iter->m_currentKey) {
		return nullptr;
	}

	auto pWrapper = CreateWrapper();

	if (iter->m_isMutable) {
		yyjson_mut_val* val = yyjson_mut_obj_iter_get_val(reinterpret_cast<yyjson_mut_val*>(iter->m_currentKey));
		if (!val) {
			return nullptr;
		}
		pWrapper->m_pDocument_mut = iter->m_pDocument_mut;
		pWrapper->m_pVal_mut = val;
	} else {
		yyjson_val* val = yyjson_obj_iter_get_val(reinterpret_cast<yyjson_val*>(iter->m_currentKey));
		if (!val) {
			return nullptr;
		}
		pWrapper->m_pDocument = iter->m_pDocument;
		pWrapper->m_pVal = val;
	}

	return pWrapper.release();
}

JsonValue* JsonManager::ObjIterGet(JsonObjIter* iter, const char* key)
{
	if (!iter || !iter->m_initialized || !key) {
		return nullptr;
	}

	auto pWrapper = CreateWrapper();

	if (iter->m_isMutable) {
		yyjson_mut_val* val = yyjson_mut_obj_iter_get(&iter->m_iterMut, key);
		if (!val) {
			return nullptr;
		}
		pWrapper->m_pDocument_mut = iter->m_pDocument_mut;
		pWrapper->m_pVal_mut = val;
	} else {
		yyjson_val* val = yyjson_obj_iter_get(&iter->m_iterImm, key);
		if (!val) {
			return nullptr;
		}
		pWrapper->m_pDocument = iter->m_pDocument;
		pWrapper->m_pVal = val;
	}

	return pWrapper.release();
}

size_t JsonManager::ObjIterGetIndex(JsonObjIter* iter)
{
	if (!iter || !iter->m_initialized) {
		return SIZE_MAX;
	}
	if (iter->m_isMutable) {
		if (iter->m_currentKey == nullptr) {
			return SIZE_MAX;
		}
		if (iter->m_iterMut.idx >= iter->m_iterMut.max) {
			return iter->m_iterMut.max - 1;
		}
		return iter->m_iterMut.idx - 1;
	} else {
		if (iter->m_iterImm.idx == 0) {
			return SIZE_MAX;
		}
		if (iter->m_iterImm.idx >= iter->m_iterImm.max) {
			return iter->m_iterImm.max - 1;
		}
		return iter->m_iterImm.idx - 1;
	}
}

bool JsonManager::ObjIterRemove(JsonObjIter* iter)
{
	if (!iter || !iter->m_isMutable) {
		return false;
	}

	return yyjson_mut_obj_iter_remove(&iter->m_iterMut) != nullptr;
}

void JsonManager::ReleaseArrIter(JsonArrIter* iter)
{
	if (iter) {
		delete iter;
	}
}

void JsonManager::ReleaseObjIter(JsonObjIter* iter)
{
	if (iter) {
		delete iter;
	}
}

HandleType_t JsonManager::GetArrIterHandleType()
{
	return g_ArrIterType;
}

HandleType_t JsonManager::GetObjIterHandleType()
{
	return g_ObjIterType;
}

JsonArrIter* JsonManager::GetArrIterFromHandle(IPluginContext* pContext, Handle_t handle)
{
	HandleError err;
	HandleSecurity sec(pContext->GetIdentity(), myself->GetIdentity());

	JsonArrIter* pIter;
	if ((err = handlesys->ReadHandle(handle, g_ArrIterType, &sec, (void**)&pIter)) != HandleError_None)
	{
		pContext->ReportError("Invalid JSONArrIter handle %x (error %d)", handle, err);
		return nullptr;
	}

	return pIter;
}

JsonObjIter* JsonManager::GetObjIterFromHandle(IPluginContext* pContext, Handle_t handle)
{
	HandleError err;
	HandleSecurity sec(pContext->GetIdentity(), myself->GetIdentity());

	JsonObjIter* pIter;
	if ((err = handlesys->ReadHandle(handle, g_ObjIterType, &sec, (void**)&pIter)) != HandleError_None)
	{
		pContext->ReportError("Invalid JSONObjIter handle %x (error %d)", handle, err);
		return nullptr;
	}

	return pIter;
}

JsonValue* JsonManager::ReadNumber(const char* dat, uint32_t read_flg, char* error, size_t error_size, size_t* out_consumed)
{
	NativeErrorBuffer::Clear(error, error_size);

	if (!dat) {
		NativeErrorBuffer::Set(error, error_size, "Invalid input data");
		return nullptr;
	}

	auto pJSONValue = CreateWrapper();
	pJSONValue->m_pDocument_mut = CreateDocument();

	if (!pJSONValue->m_pDocument_mut) {
		NativeErrorBuffer::Set(error, error_size, "Failed to create number document");
		return nullptr;
	}

	yyjson_mut_val* val = yyjson_mut_int(pJSONValue->m_pDocument_mut->get(), 0);
	if (!val) {
		NativeErrorBuffer::Set(error, error_size, "Failed to create number value");
		return nullptr;
	}

	yyjson_read_err readError{};
	const char* end_ptr = yyjson_mut_read_number(dat, val,
		static_cast<yyjson_read_flag>(read_flg), nullptr, &readError);

	if (!end_ptr || readError.code) {
		const char* msg = readError.msg ? readError.msg : "unknown error";
		NativeErrorBuffer::Set(error, error_size, "Failed to read number: %s (error code: %u, position: %zu)",
				msg, readError.code, readError.pos);
		return nullptr;
	}

	if (out_consumed) {
		*out_consumed = end_ptr - dat;
	}

	pJSONValue->m_pVal_mut = val;
	yyjson_mut_doc_set_root(pJSONValue->m_pDocument_mut->get(), val);

	return pJSONValue.release();
}

bool JsonManager::WriteNumber(JsonValue* handle, char* buffer, size_t buffer_size, size_t* out_written)
{
	if (!handle || !buffer || buffer_size == 0) {
		return false;
	}

	if (!IsNum(handle)) {
		return false;
	}

	size_t min_buffer_size = 21;
	if (handle->IsMutable()) {
		if (yyjson_mut_is_real(handle->m_pVal_mut)) {
			min_buffer_size = 40;
		}
	} else {
		if (yyjson_is_real(handle->m_pVal)) {
			min_buffer_size = 40;
		}
	}

	if (buffer_size < min_buffer_size) {
		return false;
	}

	char* result;
	if (handle->IsMutable()) {
		result = yyjson_mut_write_number(handle->m_pVal_mut, buffer);
	} else {
		result = yyjson_write_number(handle->m_pVal, buffer);
	}

	if (!result) {
		return false;
	}

	size_t written = result - buffer;
	if (written >= buffer_size) {
		return false;
	}

	if (out_written) {
		*out_written = written;
	}

	return true;
}

bool JsonManager::SetFpToFloat(JsonValue* handle, bool flt)
{
	if (!handle) {
		return false;
	}

	if (handle->IsMutable()) {
		return yyjson_mut_set_fp_to_float(handle->m_pVal_mut, flt);
	} else {
		return yyjson_set_fp_to_float(handle->m_pVal, flt);
	}
}

bool JsonManager::SetFpToFixed(JsonValue* handle, int prec)
{
	if (!handle) {
		return false;
	}

	if (prec < 1 || prec > 15) {
		return false;
	}

	if (handle->IsMutable()) {
		return yyjson_mut_set_fp_to_fixed(handle->m_pVal_mut, prec);
	} else {
		return yyjson_set_fp_to_fixed(handle->m_pVal, prec);
	}
}

bool JsonManager::SetBool(JsonValue* handle, bool value)
{
	if (!handle) {
		return false;
	}

	if (handle->IsMutable()) {
		return yyjson_mut_set_bool(handle->m_pVal_mut, value);
	} else {
		return yyjson_set_bool(handle->m_pVal, value);
	}
}

bool JsonManager::SetInt(JsonValue* handle, int value)
{
	if (!handle) {
		return false;
	}

	if (handle->IsMutable()) {
		return yyjson_mut_set_int(handle->m_pVal_mut, value);
	} else {
		return yyjson_set_int(handle->m_pVal, value);
	}
}

bool JsonManager::SetInt64(JsonValue* handle, int64_t value)
{
	if (!handle) {
		return false;
	}

	return handle->IsMutable() ? yyjson_mut_set_sint(handle->m_pVal_mut, value) : yyjson_set_sint(handle->m_pVal, value);
}

bool JsonManager::SetUint64(JsonValue* handle, uint64_t value)
{
	if (!handle) return false;
	return handle->IsMutable() ? yyjson_mut_set_uint(handle->m_pVal_mut, value) : yyjson_set_uint(handle->m_pVal, value);
}

bool JsonManager::SetDouble(JsonValue* handle, double value)
{
	if (!handle) {
		return false;
	}

	if (handle->IsMutable()) {
		return yyjson_mut_set_real(handle->m_pVal_mut, value);
	} else {
		return yyjson_set_real(handle->m_pVal, value);
	}
}

bool JsonManager::SetString(JsonValue* handle, const char* value)
{
	if (!handle || !value) {
		return false;
	}

	if (handle->IsMutable()) {
		yyjson_mut_doc* doc = handle->m_pDocument_mut->get();
		yyjson_mut_val* copied = yyjson_mut_strcpy(doc, value);
		if (!copied) {
			return false;
		}
		return yyjson_mut_set_strn(handle->m_pVal_mut,
			yyjson_mut_get_str(copied), yyjson_mut_get_len(copied));
	} else {
		return yyjson_set_str(handle->m_pVal, value);
	}
}

bool JsonManager::SetNull(JsonValue* handle)
{
	if (!handle) {
		return false;
	}

	if (handle->IsMutable()) {
		return yyjson_mut_set_null(handle->m_pVal_mut);
	} else {
		return yyjson_set_null(handle->m_pVal);
	}
}

static bool ParseInt64Variant(const char* value, std::variant<int64_t, uint64_t>* out_value, char* error, size_t error_size)
{
	NativeErrorBuffer::Clear(error, error_size);

	if (!value || !*value) {
		NativeErrorBuffer::Set(error, error_size, "Empty integer64 value");
		return false;
	}

	if (!out_value) {
		NativeErrorBuffer::Set(error, error_size, "Invalid output parameter");
		return false;
	}

	std::string_view sv(value);
	bool is_negative = (sv[0] == '-');

	if (is_negative) {
		int64_t signed_val;
		auto result = std::from_chars(sv.data(), sv.data() + sv.size(), signed_val);

		if (result.ec == std::errc{} && result.ptr == sv.data() + sv.size()) {
			*out_value = signed_val;
			return true;
		}

		if (result.ec == std::errc::result_out_of_range) {
			NativeErrorBuffer::Set(error, error_size, "Integer64 value out of range: %s", value);
		} else {
			NativeErrorBuffer::Set(error, error_size, "Invalid integer64 value: %s", value);
		}
		return false;
	}

	int64_t signed_val;
	auto result = std::from_chars(sv.data(), sv.data() + sv.size(), signed_val);

	if (result.ec == std::errc{} && result.ptr == sv.data() + sv.size()) {
		*out_value = signed_val;
		return true;
	}

	if (result.ec == std::errc::result_out_of_range) {
		uint64_t unsigned_val;
		auto unsigned_result = std::from_chars(sv.data(), sv.data() + sv.size(), unsigned_val);

		if (unsigned_result.ec == std::errc{} && unsigned_result.ptr == sv.data() + sv.size()) {
			*out_value = unsigned_val;
			return true;
		}

		if (unsigned_result.ec == std::errc::result_out_of_range) {
			NativeErrorBuffer::Set(error, error_size, "Integer64 value out of range: %s", value);
		} else {
			NativeErrorBuffer::Set(error, error_size, "Invalid integer64 value: %s", value);
		}
		return false;
	}

	NativeErrorBuffer::Set(error, error_size, "Invalid integer64 value: %s", value);
	return false;
}
