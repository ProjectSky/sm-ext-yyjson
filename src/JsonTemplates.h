#pragma once

#include <yyjson.h>
#include <IJsonManager.h>

// Forward declaration
class JsonValue;

class JsonTemplates {
private:
	template<typename T>
	struct TypeTraits;

public:
	JsonTemplates() = delete;

	// ============================================================================
	// Generic Getter Template for Object/Array/Pointer Access
	// ============================================================================

	template<typename T, typename AccessorMut, typename AccessorImm>
	static bool GetValueTemplate(
		JsonValue* handle,
		AccessorMut accessor_mut,
		AccessorImm accessor_imm,
		T* out_value)
	{
		if (!handle || !out_value) {
			return false;
		}

		if (handle->IsMutable()) {
			yyjson_mut_val* val = accessor_mut();
			if (!val || !TypeTraits<T>::is_type_mut(val)) {
				return false;
			}
			*out_value = TypeTraits<T>::get_value_mut(val);
			return true;
		} else {
			yyjson_val* val = accessor_imm();
			if (!val || !TypeTraits<T>::is_type_imm(val)) {
				return false;
			}
			*out_value = TypeTraits<T>::get_value_imm(val);
			return true;
		}
	}

	// ============================================================================
	// Generic Setter Template for Object Operations
	// ============================================================================

	template<typename T>
	static bool ObjectSetTemplate(JsonValue* handle, const char* key, T value)
	{
		if (!handle || !handle->IsMutable() || !key) {
			return false;
		}

		yyjson_mut_doc* doc = handle->m_pDocument_mut->get();
		return yyjson_mut_obj_put(
			handle->m_pVal_mut,
			yyjson_mut_strcpy(doc, key),
			TypeTraits<T>::create_value(doc, value)
		);
	}

	// ============================================================================
	// Generic Array Replace Template
	// ============================================================================

	template<typename T>
	static bool ArrayReplaceTemplate(JsonValue* handle, size_t index, T value)
	{
		if (!handle || !handle->IsMutable()) {
			return false;
		}

		size_t arr_size = yyjson_mut_arr_size(handle->m_pVal_mut);
		if (index >= arr_size) {
			return false;
		}

		yyjson_mut_doc* doc = handle->m_pDocument_mut->get();
		return yyjson_mut_arr_replace(
			handle->m_pVal_mut,
			index,
			TypeTraits<T>::create_value(doc, value)
		) != nullptr;
	}

	// Specialization for null
	static bool ArrayReplaceNull(JsonValue* handle, size_t index)
	{
		if (!handle || !handle->IsMutable()) {
			return false;
		}

		size_t arr_size = yyjson_mut_arr_size(handle->m_pVal_mut);
		if (index >= arr_size) {
			return false;
		}

		return yyjson_mut_arr_replace(
			handle->m_pVal_mut,
			index,
			yyjson_mut_null(handle->m_pDocument_mut->get())
		) != nullptr;
	}

	// Specialization for string
	static bool ArrayReplaceString(JsonValue* handle, size_t index, const char* value)
	{
		if (!handle || !handle->IsMutable() || !value) {
			return false;
		}

		size_t arr_size = yyjson_mut_arr_size(handle->m_pVal_mut);
		if (index >= arr_size) {
			return false;
		}

		return yyjson_mut_arr_replace(
			handle->m_pVal_mut,
			index,
			yyjson_mut_strcpy(handle->m_pDocument_mut->get(), value)
		) != nullptr;
	}

	// ============================================================================
	// Generic Array Append Template
	// ============================================================================

	template<typename T>
	static bool ArrayAppendTemplate(JsonValue* handle, T value)
	{
		if (!handle || !handle->IsMutable()) {
			return false;
		}

		return yyjson_mut_arr_append(
			handle->m_pVal_mut,
			TypeTraits<T>::create_value(handle->m_pDocument_mut->get(), value)
		);
	}

	// Specialization for null
	static bool ArrayAppendNull(JsonValue* handle)
	{
		if (!handle || !handle->IsMutable()) {
			return false;
		}

		return yyjson_mut_arr_append(
			handle->m_pVal_mut,
			yyjson_mut_null(handle->m_pDocument_mut->get())
		);
	}

	// Specialization for string
	static bool ArrayAppendString(JsonValue* handle, const char* value)
	{
		if (!handle || !handle->IsMutable() || !value) {
			return false;
		}

		return yyjson_mut_arr_append(
			handle->m_pVal_mut,
			yyjson_mut_strcpy(handle->m_pDocument_mut->get(), value)
		);
	}

	// ============================================================================
	// Generic Array Prepend Template
	// ============================================================================

	template<typename T>
	static bool ArrayPrependTemplate(JsonValue* handle, T value)
	{
		if (!handle || !handle->IsMutable()) {
			return false;
		}

		return yyjson_mut_arr_prepend(
			handle->m_pVal_mut,
			TypeTraits<T>::create_value(handle->m_pDocument_mut->get(), value)
		);
	}

	// Specialization for null
	static bool ArrayPrependNull(JsonValue* handle)
	{
		if (!handle || !handle->IsMutable()) {
			return false;
		}

		return yyjson_mut_arr_prepend(
			handle->m_pVal_mut,
			yyjson_mut_null(handle->m_pDocument_mut->get())
		);
	}

	// Specialization for string
	static bool ArrayPrependString(JsonValue* handle, const char* value)
	{
		if (!handle || !handle->IsMutable() || !value) {
			return false;
		}

		return yyjson_mut_arr_prepend(
			handle->m_pVal_mut,
			yyjson_mut_strcpy(handle->m_pDocument_mut->get(), value)
		);
	}

	// ============================================================================
	// Generic Array Insert Template
	// ============================================================================

	template<typename T>
	static bool ArrayInsertTemplate(JsonValue* handle, size_t index, T value)
	{
		if (!handle || !handle->IsMutable()) {
			return false;
		}

		size_t arr_size = yyjson_mut_arr_size(handle->m_pVal_mut);
		if (index > arr_size) {
			return false;
		}

		return yyjson_mut_arr_insert(
			handle->m_pVal_mut,
			TypeTraits<T>::create_value(handle->m_pDocument_mut->get(), value),
			index
		);
	}

	// Specialization for null
	static bool ArrayInsertNull(JsonValue* handle, size_t index)
	{
		if (!handle || !handle->IsMutable()) {
			return false;
		}

		size_t arr_size = yyjson_mut_arr_size(handle->m_pVal_mut);
		if (index > arr_size) {
			return false;
		}

		return yyjson_mut_arr_insert(
			handle->m_pVal_mut,
			yyjson_mut_null(handle->m_pDocument_mut->get()),
			index
		);
	}

	// Specialization for string
	static bool ArrayInsertString(JsonValue* handle, size_t index, const char* value)
	{
		if (!handle || !handle->IsMutable() || !value) {
			return false;
		}

		size_t arr_size = yyjson_mut_arr_size(handle->m_pVal_mut);
		if (index > arr_size) {
			return false;
		}

		return yyjson_mut_arr_insert(
			handle->m_pVal_mut,
			yyjson_mut_strcpy(handle->m_pDocument_mut->get(), value),
			index
		);
	}

	// ============================================================================
	// Type Checking Template
	// ============================================================================

	template<typename MutCheckFunc, typename ImmCheckFunc>
	static bool CheckTypeTemplate(JsonValue* handle, MutCheckFunc mut_check, ImmCheckFunc imm_check)
	{
		if (!handle) {
			return false;
		}

		if (handle->IsMutable()) {
			return mut_check(handle->m_pVal_mut);
		} else {
			return imm_check(handle->m_pVal);
		}
	}
};

// Type traits specializations.
template<>
struct JsonTemplates::TypeTraits<bool> {
	static bool is_type_mut(yyjson_mut_val* val) { return yyjson_mut_is_bool(val); }
	static bool is_type_imm(yyjson_val* val) { return yyjson_is_bool(val); }
	static bool get_value_mut(yyjson_mut_val* val) { return yyjson_mut_get_bool(val); }
	static bool get_value_imm(yyjson_val* val) { return yyjson_get_bool(val); }
	static yyjson_mut_val* create_value(yyjson_mut_doc* doc, bool value) {
		return yyjson_mut_bool(doc, value);
	}
};

template<>
struct JsonTemplates::TypeTraits<int> {
	static bool is_type_mut(yyjson_mut_val* val) { return yyjson_mut_is_int(val); }
	static bool is_type_imm(yyjson_val* val) { return yyjson_is_int(val); }
	static int get_value_mut(yyjson_mut_val* val) { return yyjson_mut_get_int(val); }
	static int get_value_imm(yyjson_val* val) { return yyjson_get_int(val); }
	static yyjson_mut_val* create_value(yyjson_mut_doc* doc, int value) {
		return yyjson_mut_int(doc, value);
	}
};

template<>
struct JsonTemplates::TypeTraits<double> {
	static bool is_type_mut(yyjson_mut_val* val) { return yyjson_mut_is_num(val); }
	static bool is_type_imm(yyjson_val* val) { return yyjson_is_num(val); }
	static double get_value_mut(yyjson_mut_val* val) { return yyjson_mut_get_num(val); }
	static double get_value_imm(yyjson_val* val) { return yyjson_get_num(val); }
	static yyjson_mut_val* create_value(yyjson_mut_doc* doc, double value) {
		return yyjson_mut_real(doc, value);
	}
};
