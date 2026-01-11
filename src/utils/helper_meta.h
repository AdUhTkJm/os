#ifndef HELPER_META_H
#define HELPER_META_H

// Meta-programming.

namespace os {

template<class T, T v>
struct integral_constant {
  constexpr static T value = v;
};

using true_type = integral_constant<bool, true>;
using false_type = integral_constant<bool, false>;

namespace detail {
  template<typename T>
  struct type_identity { using type = T; };

  template<typename T>
  auto try_add_lvalue_reference(int) -> type_identity<T&>;
  template<typename T>
  auto try_add_lvalue_reference(...) -> type_identity<T>;

  template<typename T>
  auto try_add_rvalue_reference(int) -> type_identity<T&&>;
  template<typename T>
  auto try_add_rvalue_reference(...) -> type_identity<T>;

  template<typename B>
  true_type test_ptr_conv(const volatile B*);
  template<typename>
  false_type test_ptr_conv(const volatile void*);

  template<typename B, typename D>
  auto test_is_base_of(int) -> decltype(test_ptr_conv<B>(static_cast<D*>(nullptr)));
  template<typename, typename>
  auto test_is_base_of(...) -> true_type;

} // namespace detail
 
template<typename T>
struct add_lvalue_reference : decltype(detail::try_add_lvalue_reference<T>(0)) {};
 
template<typename T>
struct add_rvalue_reference : decltype(detail::try_add_rvalue_reference<T>(0)) {};

template<typename T>
T declval() noexcept {
  static_assert(false, "declval not allowed in an evaluated context");
}

template<typename T, typename U>
struct is_same : false_type { };
template<typename T>
struct is_same<T, T> : true_type { };

template<typename T, typename U>
constexpr bool is_same_v = is_same<T, U>::value;

template<typename T, typename U>
concept same_as = is_same_v<T, U>;

template<typename T>
struct is_integral {
  constexpr static bool value = requires (T t, T *p, void (*f)(T)) {
    f(0);
    reinterpret_cast<T>(t);
    p + t;
  };
};

template<typename T>
constexpr bool is_integral_v = is_integral<T>::value;

template<typename T>
struct is_floating_point : integral_constant<bool, is_same_v<T, float> || is_same_v<T, double> || is_same_v<T, long double>> {};

template<typename T>
constexpr bool is_floating_point_v = is_floating_point<T>::value;

template<typename T>
struct is_pointer : false_type { };

template<typename T>
struct is_pointer<T *> : true_type { };

template<typename T>
struct is_pointer<T * const> : true_type { };

template<typename T>
struct is_pointer<T * const volatile> : true_type { };

template<typename T>
struct is_pointer<T * volatile> : true_type { };

template<typename T>
constexpr bool is_pointer_v = is_pointer<T>::value;

// Note that std::nullptr_t is no longer there.
template<typename T>
struct is_scalar : integral_constant<bool, __is_enum(T) || is_integral_v<T> || is_floating_point_v<T> || is_pointer_v<T>> {};

template<typename T>
struct is_class : integral_constant<bool, __is_class(T)> {};

template<typename T>
constexpr bool is_class_v = is_class<T>::value;

template<typename T>
constexpr bool is_scalar_v = is_scalar<T>::value;

template<typename T>
constexpr bool is_pod_v = __is_standard_layout(T) && __is_trivial(T);

template<typename T> struct remove_pointer { using type = T; };
template<typename T> struct remove_pointer<T*> { using type = T; };
template<typename T> struct remove_pointer<T* const> { using type = T; };
template<typename T> struct remove_pointer<T* volatile> { using type = T; };
template<typename T> struct remove_pointer<T* const volatile> { using type = T; };
template<typename T> using remove_pointer_t = remove_pointer<T>::type;

template<typename T> struct remove_reference { using type = T; };
template<typename T> struct remove_reference<T&> { using type = T; };
template<typename T> struct remove_reference<T&&> { using type = T; };
template<typename T> using remove_reference_t = typename remove_reference<T>::type;

template<typename T>
constexpr T&& forward(remove_reference_t<T>& arg) noexcept {
  return static_cast<T&&>(arg);
}
template<typename T>
constexpr T&& forward(remove_reference_t<T>&& arg) noexcept {
  return static_cast<T&&>(arg);
}

template<typename T>
constexpr remove_reference_t<T> &&move(T &&arg) noexcept {
  return (remove_reference_t<T>&&) arg;
}
 
template<typename Base, typename Derived>
struct is_base_of : integral_constant<
 bool, is_class_v<Base> && is_class_v<Derived> && decltype(detail::test_is_base_of<Base, Derived>(0))::value
> {};

template<bool B, typename T, typename U>
struct conditional { using type = T; };

template<typename T, typename U>
struct conditional<false, T, U> { using type = U; };

template<bool B, typename T, typename U>
using conditional_t = conditional<B, T, U>::type;

template<class T>
struct is_enum : integral_constant<bool, __is_enum(T)> {};

template<class T>
struct underlying_type { using type = __underlying_type(T); };

template<class T> struct is_const : false_type {};
template<class T> struct is_const<const T> : true_type {};


template<class T> struct is_reference : false_type {};
template<class T> struct is_reference<T&> : true_type {};
template<class T> struct is_reference<T&&> : true_type {};

template<class T>
struct is_function : integral_constant<
  bool,
  !is_const<const T>::value && !is_reference<T>::value
> {};

template<class T>
constexpr bool is_function_v = is_function<T>::value;

template<unsigned long ...N>
struct index_sequence {
  using value_type = unsigned long;
  constexpr static unsigned long size() { return sizeof...(N); }
};

namespace detail {
  template<unsigned long N, unsigned long ...Next>
  struct make_index_sequence_helper : public make_index_sequence_helper<N - 1U, N - 1U, Next...> {};

  template<unsigned long... Next>
  struct make_index_sequence_helper<0U, Next...> {
    using type = index_sequence<Next...>;
  };
} // namespace detail

// 4. The user-facing alias template.
// This is the public interface that leverages the helper to return the correct sequence type.
template <unsigned long N>
using make_index_sequence = typename detail::make_index_sequence_helper<N>::type;

}

#endif
