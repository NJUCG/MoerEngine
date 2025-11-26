#ifndef MOER_ENGINE_SINGLETON_H
#define MOER_ENGINE_SINGLETON_H
namespace Moer {
template<typename T>
class Singleton {
public:
    inline static T& GetInstance() {
        static T instance;
        return instance;
    }

    Singleton(const Singleton&)            = delete;
    Singleton& operator=(const Singleton&) = delete;

protected:
    Singleton()          = default;
    virtual ~Singleton() = default;
};
} // namespace Moer

#endif //MOER_ENGINE_SINGLETON_H