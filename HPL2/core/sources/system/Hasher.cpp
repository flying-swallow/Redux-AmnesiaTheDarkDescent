#include "system/Hasher.h"

#include <random>

hash_t hash_random()
{
	static thread_local std::mt19937_64 rng{ std::random_device{}() };
	static thread_local std::uniform_int_distribution<uint64_t> dist;
	return dist( rng );
}
