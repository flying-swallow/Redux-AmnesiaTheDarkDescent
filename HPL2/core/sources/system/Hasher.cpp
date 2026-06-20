#include "system/Hasher.h"

#include <random>

hash_t hash_random()
{
	static thread_local std::minstd_rand0 rng{ static_cast<unsigned int>(std::random_device{}()) };
	static thread_local std::uniform_int_distribution<uint64_t> dist;
	return dist( rng );
}
