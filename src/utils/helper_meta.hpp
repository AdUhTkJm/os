#ifndef HELPER_META_H
#define HELPER_META_H

// Meta-programming.

namespace os {

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

} // namespace detail
 
template<typename T>
struct add_lvalue_reference : decltype(detail::try_add_lvalue_reference<T>(0)) {};
 
template<typename T>
struct add_rvalue_reference : decltype(detail::try_add_rvalue_reference<T>(0)) {};

template<typename T>
add_rvalue_reference<T> declval() noexcept {
  static_assert(false, "declval not allowed in an evaluated context");
}

template<typename T, typename U>
struct is_same {
  constexpr static bool value = false;
};
template<typename T>
struct is_same<T, T> {
  constexpr static bool value = true;
};

template<typename T, typename U>
constexpr bool is_same_v = is_same<T, U>::value;

template<typename T, typename U>
concept same_as = is_same_v<T, U>;

}

#endif
