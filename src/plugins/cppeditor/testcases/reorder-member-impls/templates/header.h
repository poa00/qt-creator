template <typename T> struct S {
    void firstFunction();
    template<typename U> void secondFunction(U);
    template<typename U> void irrelevantFunction(U) {}
    void thirdFunction();
};

template<typename T> template<typename U> void S<T>::secondFunction(U) {}
template<typename T> void S<T>::thirdFunction() {}
template<typename T> void S<T>::firstFunction() {}
