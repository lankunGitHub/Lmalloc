#include "witness.h"

void witness_init(witness_t* witness,
                  const char* name,
                  witness_rank_t rank,
                  witness_comp_t* comp,
                  void* opaque)
{
    witness->name = name;
    witness->rank = rank;
    witness->comp = comp;
    witness->opaque = opaque;
}

void witnesses_cleanup(witness_tsd_t* witness_tsd)
{
    witness_assert_lockless(witness_tsd_tsdn(witness_tsd));
}

void witness_prefork(witness_tsd_t* witness_tsd)
{
    witness_tsd->forking = true;
}

void witness_postfork_parent(witness_tsd_t* witness_tsd)
{
    witness_tsd->forking = false;
}

void witness_postfork_child(witness_tsd_t* witness_tsd)
{
    witness_tsd->forking = false;
}