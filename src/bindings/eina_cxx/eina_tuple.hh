#ifndef EFL_EINA_EINA_TUPLE_HH_
#define EFL_EINA_EINA_TUPLE_HH_

#include <eina_integer_sequence.hh>

#include <tuple>

namespace efl { namespace eina { namespace _mpl {

template <typename A, typename... Args>
struct push_back;

template <template <typename... Args> class C, typename... Args, typename... AArgs>
struct push_back<C<Args...>, AArgs...>
{
  typedef C<Args..., AArgs...> type;
};

template <typename A, typename... Args>
struct push_front;

template <template <typename... Args> class C, typename... Args, typename... AArgs>
struct push_front<C<Args...>, AArgs...>
{
  typedef C<Args..., AArgs...> type;
};

template <typename A, std::size_t N = 1>
struct pop_front;
      
template <template <typename...> class C, typename T, typename... Args>
struct pop_front<C<T, Args...>, 1>
{
  typedef C<Args...> type;
};

template <typename A, std::size_t N>
struct pop_front : pop_front<typename pop_front<A, 1>::type, N-1>
{
};

template <typename T, typename F, std::size_t... I>
void for_each_impl(T&& t, F&& f, eina::index_sequence<I...>)
{
  std::initializer_list<int> l = { (f(std::get<I>(t)), 0)...};
  static_cast<void>(l);
}

template <typename T, typename F>
void for_each(T&& t, F&& f)
{
  _mpl::for_each_impl(t, f, eina::make_index_sequence
                      <std::tuple_size<typename std::remove_reference<T>::type>::value>());
}

} } }

#endif
