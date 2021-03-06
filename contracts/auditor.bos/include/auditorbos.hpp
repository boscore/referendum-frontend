#include <eosio/eosio.hpp>
#include <eosio/multi_index.hpp>
#include <eosio/system.hpp>
#include <eosio/singleton.hpp>
#include <eosio/time.hpp>

#include "external_types.hpp"
#include "voter_info.hpp"

const name TOKEN_CONTRACT = "eosio.token"_n;
const name AUDITORS_PERMISSION = "auditors"_n;

using namespace eosio;
using namespace std;

/**
 * - `lockupasset` (asset) -  The amount of assets that are locked up by each candidate applying for election.
 * - `maxvotes` (int default=3) - The maximum number of votes that each member can make for a candidate.
 * - `numelected` (int default=5) -  Number of auditors to be elected for each election count.
 * - `authaccount` ( account= "auditor.bos") - account to have active auth set with all auditors on the newtenure.
 * - `auth_threshold_auditors` (uint8) - Number of auditors required to approve the lowest level actions.
 * - `lockup_release_time_delay` (date) - The time before locked up stake can be released back to the candidate using the unstake action
 */
struct [[eosio::table("config"), eosio::contract("auditorbos")]] contr_config {
    // The amount of assets that are locked up by each candidate applying for election.
    asset lockupasset;

    // The maximum number of votes that each member can make for a candidate.
    uint8_t maxvotes = 3;

    // Number of auditors to be elected for each election count.
    uint8_t numelected = 5;

    // account to have active auth set with all all auditors on the newtenure.
    name authaccount = name{0};

    // required number of auditors required to approve different levels of authenticated actions.
    uint8_t auth_threshold_auditors;

    // The time before locked up stake can be released back to the candidate using the unstake action
    uint32_t lockup_release_time_delay;
};

typedef singleton<"config"_n, contr_config> config_table;

/**
 * - `candidate_name` (name) - Account name of the candidate
 * - `locked_tokens` (asset) - An asset object representing the number of tokens locked when registering
 * - `total_votes` (uint64) - Updated tally of the number of votes cast to a candidate. This is updated and used as part of the `newtenure` calculations. It is updated every time there is a vote change or a change of token balance for a voter for this candidate to facilitate live voting stats.
 * - `is_active` (bool) - Boolean indicating if the candidate is currently available for election.
 * - `unstaking_end_time_stamp` (time_point_sec) - timestamp that user is allowed to unstake tokens.
 */
struct [[eosio::table("candidates"), eosio::contract("auditorbos")]] candidates_row {
    name candidate_name;
    asset locked_tokens;
    uint64_t total_votes;
    bool is_active;
    time_point_sec unstaking_end_time_stamp;

    uint64_t primary_key() const { return candidate_name.value; }
};

typedef multi_index<"candidates"_n, candidates_row> candidates_table;

/**
 * - `auditor_name` (name) - Account name of the auditor
 * - `total_votes` - Tally of the number of votes cast to a auditor when they were elected in. This is updated as part of the `newtenure` action.
 */
struct [[eosio::table("auditors"), eosio::contract("auditorbos")]] auditors_row {
    name auditor_name;

    uint64_t primary_key() const { return auditor_name.value; }
};

typedef multi_index<"auditors"_n, auditors_row> auditors_table;

/**
 * - `candidate_name` (name) - Account name of candidate
 * - `bio` (string JSON) - Bio of candidate
 */
struct [[eosio::table("bios"), eosio::contract("auditorbos")]] bios_row {
    name candidate_name;
    string bio;

    uint64_t primary_key() const { return candidate_name.value; }
};

typedef multi_index<"bios"_n, bios_row > bios_table;

/**
 * - `voter` (name) - The account name of the voter
 * - `proxy` (name) - DEPRECATED: the proxy set by the voter, if any (not being used to count final vote tally)
 * - `staked` (uint64) - DEPRECATED: total staked amount of voter (not being used to count final vote tally)
 * - `candidates` (name[]) - The candidates voted for, can supply up to the maximum number of votes (currently 5) - Can be configured via `updateconfig`
 */
struct [[eosio::table("votes"), eosio::contract("auditorbos")]] votes_row {
    name voter;
    name proxy;
    uint64_t staked;
    vector<name> candidates;

    uint64_t primary_key() const { return voter.value; }
};

typedef eosio::multi_index<"votes"_n, votes_row> votes_table;

/**
 * - `voter` (name) - The account name of the voter
 * - `vote_json` (string JSON)- JSON metadata for voting (ex: {"comment": "great work"})
 * - `updated_at` (time_point_sec) last updated timestamp
 */
struct [[eosio::table("votejson"), eosio::contract("auditorbos")]] votejson_row {
    name voter;
    string vote_json;
    time_point_sec updated_at;

    uint64_t primary_key() const { return voter.value; }
};

typedef eosio::multi_index<"votejson"_n, votejson_row> votejson_table;

class auditorbos : public contract {

private: // Variables used throughout the other actions.
    config_table        _config;
    candidates_table    _candidates;
    votes_table         _votes;
    votejson_table      _votejson;
    bios_table          _bios;
    auditors_table      _auditors;
    voter_table         _voters;

public:

    auditorbos( name s, name code, datastream<const char*> ds )
        :contract(s,code,ds),
            _candidates(_self, _self.value),
            _votes(_self, _self.value),
            _bios(_self, _self.value),
            _auditors(_self, _self.value),
            _config(_self, _self.value),
            _votejson(_self, _self.value),
            _voters( "eosio"_n, "eosio"_n.value )
    {}

    /**
     * ### updateconfig
     *
     * Updates the contract configuration parameters to allow changes without needing to redeploy the source code.
     *
     * #### Message
     *
     * updateconfig(<params>)
     *
     * This action asserts:
     *
     * - the message has the permission of the contract account.
     * - the supplied asset symbol matches the current lockup symbol if it has been previously set or that there have been no 	.
     *
     * The parameters are:
     *
     * - lockupasset(uint8_t) : defines the asset and amount required for a user to register as a candidate. This is the amount that will be locked up until the user calls `withdrawcand` in order to get the asset returned to them. If there are currently already registered candidates in the contract this cannot be changed to a different asset type because of introduced complexity of handling the staked amounts.
     * - maxvotes(asset) : Defines the maximum number of candidates a user can vote for at any given time.
     * - numelected(uint16_t) : The number of candidates to elect for auditors. This is used for the payment amount to auditors for median amount.
     * - authaccount(name) : The managing account that controls the BOS auditor permission.
     * - auth_threshold_auditors (uint8) : The number of auditors required to approve an action in the low permission category ( ordinary action such as a worker proposal).
     */
    [[eosio::action]]
    void updateconfig( const contr_config newconfig );

    /**
     * Action to listen to from the associated token contract to ensure registering should be allowed.
     *
     * @param from The account to observe as the source of funds for a transfer
     * @param to The account to observe as the destination of funds for a transfer
     * @param quantity
     * @param memo A string to attach to a transaction. For staking this string should match the name of the running contract eg "auditor.bos". Otherwise it will be regarded only as a generic transfer to the account.
     * This action is intended only to observe transfers that are run by the associated token contract for the purpose of tracking the moving weights of votes if either the `from` or `to` in the transfer have active votes. It is not included in the ABI to prevent it from being called from outside the chain.
     */
    [[eosio::on_notify("eosio.token::transfer")]]
    void transfer( name from,
                   name to,
                   asset quantity,
                   const std::string& memo );

    /**
     * This action is used to nominate a candidate for auditor elections.
     * It must be authorised by the candidate and the candidate must be an active member of BOS, having agreed to the latest constitution.
     * The candidate must have transferred a quantity of tokens (determined by a config setting - `lockupasset`) to the contract for staking before this action is executed. This could have been from a recent transfer with the contract name in the memo or from a previous time when this account had nominated, as long as the candidate had never `unstake`d those tokens.
     *
     * ### Assertions:
     * - The account performing the action is authorised.
     * - The candidate is not already a nominated candidate.
     * - The requested pay amount is not more than the config max amount
     * - The requested pay symbol type is the same from config max amount ( The contract supports only one token symbol for payment)
     * - The candidate is currently a member or has agreed to the latest constitution.
     * - The candidate has transferred sufficient funds for staking if they are a new candidate.
     * - The candidate has enough staked if they are re-nominating as a candidate and the required stake has changed since they last nominated.
     *
     * @param cand - The account id for the candidate nominating.
     *
     *
     * ### Post Condition:
     * The candidate should be present in the candidates table and be set to active. If they are a returning candidate they should be set to active again. The `locked_tokens` value should reflect the total of the tokens they have transferred to the contract for staking. The number of active candidates in the contract will incremented.
     */
    [[eosio::action]]
    void nominatecand( const name cand);

    /**
     * This action is used to withdraw a candidate from being active for auditor elections.
     *
     * ### Assertions:
     * - The account performing the action is authorised.
     * - The candidate is already a nominated candidate.
     *
     * @param cand - The account id for the candidate nominating.
     *
     *
     * ### Post Condition:
     * The candidate should still be present in the candidates table and be set to inactive. If the were recently an elected auditor there may be a time delay on when they can unstake their tokens from the contract. If not they will be able to unstake their tokens immediately using the unstake action.
     */
    [[eosio::action]]
    void withdrawcand( const name cand);

    /**
     * This action is used to remove a candidate from being a candidate for auditor elections.
     *
     * ### Assertions:
     * - The action is authorised by the mid level permission the auth account for the contract.
     * - The candidate is already a nominated candidate.
     *
     * @param cand - The account id for the candidate nominating.
     *
     * ### Post Condition:
     * The candidate should still be present in the candidates table and be set to inactive. If the `lockupstake` parameter is true the stake will be locked until the time delay has passed. If not the candidate will be able to unstake their tokens immediately using the unstake action to have them returned.
     */
    [[eosio::action]]
    void firecand( const name cand );

    /**
     * This action is used to resign as a auditor.
     *
     * ### Assertions:
     * - The `auditor` account performing the action is authorised to do so.
     * - The `auditor` account is currently an elected auditor.
     *
     * @param auditor - The account id for the candidate nominating.
     *
     *
     * ### Post Condition:
     * The auditor will be removed from the active auditors and should still be present in the candidates
     * table but will be set to inactive. Their staked tokens will be locked up for the time delay added from
     * the moment this action was called so they will not able to unstake until that time has passed.
     *
     * A replacement auditor will selected from the candidates to fill the missing place (based on vote ranking)
     * then the auths for the controlling BOS auth account will be set for the auditor board.
     */
    [[eosio::action]]
    void resign( const name auditor );

    /**
     * This action is used to remove a auditor.
     *
     * ### Assertions:
     * - The action is authorised by the mid level of the auth account (currently elected auditor board).
     * - The `auditor` account is currently an elected auditor.
     *
     * @param auditor - The account id for the candidate nominating.
     *
     *
     * ### Post Condition:
     * The auditor will be removed from the active auditors and should still be present in the candidates table but will be set to inactive.
     * Their staked tokens will be locked up for the time delay added from the moment this action was called so they will not able to unstake until
     * that time has passed. A replacement auditor will selected from the candidates to fill the missing place (based on vote ranking)
     * then the auths for the controlling BOS auth account will be set for the auditor board.
     */
    [[eosio::action]]
    void fireauditor( const name auditor );

    /**
     * This action is used to update the bio for a candidate.
     *
     * ### Assertions:
     * - The `cand` account performing the action is authorised to do so.
     * - The string in the bio field is less than 256 characters.
     *
     * @param cand - The account id for the candidate nominating.
     * @param bio - A string of bio data that will be passed through the contract.
     *
     *
     * ### Post Condition:
     * Nothing from this action is stored on the blockchain. It is only intended to ensure authentication of changing the bio which will be stored off chain.
     */
    [[eosio::action]]
    void updatebio( const name cand, const string bio );

    /**
     * This action is to facilitate voting for candidates to become auditors of BOS.
     * Each member will be able to vote a configurable number of auditors set by the contract configuration.
     * When a voter calls this action either a new vote will be recorded or the existing vote for that voter will be modified.
     * If an empty array of candidates is passed to the action an existing vote for that voter will be removed.
     *
     * ### Assertions:
     * - The voter account performing the action is authorised to do so.
     * - The voter account performing has agreed to the latest member terms for BOS.
     * - The number of candidates in the newvotes vector is not greater than the number of allowed votes per voter as set by the contract config.
     * - Ensure there are no duplicate candidates in the voting vector.
     * - Ensure all the candidates in the vector are registered and active candidates.
     *
     * @param voter - The account id for the voter account.
     * @param candidates - A vector of account ids for the candidate that the voter is voting for.
     *
     * ### Post Condition:
     * An active vote record for the voter will have been created or modified to reflect the candidates.
     */
    [[eosio::action]]
    void vote( const name voter,
               const vector<name> candidates,
               const string& vote_json );

    /**
     * Removes existing vote from {{ voter }}.
     */
    [[eosio::action]]
    void unvote( const name voter );

    /**
     * ### newtenure
     *
     * This action is to be run to end and begin each period in BOS life cycle. It performs multiple tasks for BOS including:
     *
     * - Allocate auditors from the candidates tables based on those with most votes at the moment this action is run. -- This action removes and selects a full set of auditors each time it is successfully run selected from the candidates with the most votes weight. If there are not enough eligible candidates to satisfy BOS config numbers the action adds the highest voted candidates as auditors as long their votes weight is greater than 0. At this time the held stake for the departing auditors is set to have a time delayed lockup to prevent the funds from releasing too soon after each auditor has been in office.
     * - Distribute pay for the existing auditors based on the configs into the pending pay table so it can be claimed by individual candidates. -- The pay is distributed as determined by the median pay of the currently elected auditors. Therefore all elected auditors receive the same pay amount.
     * - Set BOS auths for the intended controlling accounts based on the configs thresholds with the newly elected auditors. This action asserts unless the following conditions have been met:
     * - The action cannot be called multiple times within the period since the last time it was previously run successfully. This minimum time between allowed calls is configured by the period length parameter in contract configs.
     * - To run for the first time a minimum threshold of voter engagement must be satisfied. This is configured by the `initial_vote_quorum_percent` field in the contract config with the percentage calculated from the amount of registered votes cast by voters against the max supply of tokens for BOS's primary currency.
     * - After the initial vote quorum percent has been reached subsequent calls to this action will require a minimum of `vote_quorum_percent` to vote for the votes to be considered sufficient to trigger a new period with new auditors.
     *
     * ##### Parameters:
     *
     * candidates - auditor candidates to be nominated as auditors
     * message - a string that is used to log a message in the chain history logs. It serves no function in the contract logic.
     */
    [[eosio::action]]
    void newtenure( const vector<name> candidates, const string message);

    /**
     * This action is used to unstake a candidates tokens and have them transferred to their account.
     *
     * ### Assertions:
     * - The candidate was a nominated candidate at some point in the passed.
     * - The candidate is not already a nominated candidate.
     * - The tokens held under candidate's account are not currently locked in a time delay.
     *
     * @param cand - The account id for the candidate nominating.
     *
     *
     * ### Post Condition:
     * The candidate should still be present in the candidates table and should be still set to inactive. The candidates tokens will be transferred back to their account and their `locked_tokens` value will be reduced to 0.
     */
    [[eosio::action]]
    void unstake( const name cand );

    /**
     * ### ACTION `cleancand`
     *
     * > Used to refresh `candidate` data entry
     *
     * > Authorized by `require_auth( _self )`
     *
     * - set `total_votes` to 0
     * - set `is_active` if locked_tockens met minimum threshold
     */
    [[eosio::action]]
    void cleancand( const name cand );

    /**
     * ### ACTION `cleanvoter`
     *
     * > Used to refresh `voter` data entry
     *
     * > Authorized by `require_auth( _self )`
     *
     * - If voter has not voted for any candidates, remove voter from `votes` & `votejson`
     * - Update voter's staked & proxy data
     * - Add `vote_json` if not present in `votejson` table
     */
    [[eosio::action]]
    void cleanvoter( const name voter );


private: // Private helper methods used by other actions.

    contr_config configs();

    void set_auditor_auths();

    void remove_auditor( const name auditor );

    void invalid_candidate( const name cand, const bool lockupStake );

    void allocate_auditors( const vector<name> candidates );

    void remove_voter( const name voter );

    // Do not use directly, use the VALIDATE_JSON macro instead!
    void validate_json(
        const string& payload,
        size_t max_size,
        const char* not_object_message,
        const char* over_size_message
    );
};
