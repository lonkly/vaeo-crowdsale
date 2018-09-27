#include "crowdsale.hpp"
#include "override.hpp"

#define EOS2TKN(EOS) (int64_t)((EOS) * POW10(DECIMALS) * RATE / (1.0 * POW10(4) * RATE_DENOM))

#ifdef DEBUG
#define NOW this->state.time
#else
#define NOW now()
#endif

crowdsale::crowdsale(account_name self) :
	eosio::contract(self),
	state_singleton(this->_self, this->_self),
	whitelist(this->_self, this->_self),
	issuer(eosio::string_to_name(STR(ISSUER))),
	state(state_singleton.exists() ? state_singleton.get() : default_parameters()),
	toclean(false)
{
}

crowdsale::~crowdsale() {
	if (toclean) {
		this->state_singleton.remove();
	} else {
		this->state_singleton.set(this->state, this->_self);
	}
}

void crowdsale::on_deposit(account_name investor, eosio::extended_asset quantity) {
	eosio_assert(this->state_singleton.exists(), "Not initialized");
	eosio_assert(NOW >= this->state.start, "Crowdsale not started");
	eosio_assert(NOW <= this->state.finish, "Crowdsale finished");
	eosio_assert(NOW <= this->state.valid_until, "Rates not updated yet");

	eosio_assert(!this->state.hardcap_reached, "Hardcap reached");

	eosio_assert(quantity.amount >= MIN_CONTRIB, "Contribution too low");
	eosio_assert((quantity.amount <= MAX_CONTRIB) || !MAX_CONTRIB, "Contribution too high");

	if (this->whitelist.find(investor) != this->whitelist.end()) {
		this->state.total_eos += quantity;
	}

	deposits deposits_table(this->_self, investor);
	deposits_table.emplace(this->_self, [&](auto& deposit) {
		deposit.pk = deposits_table.available_primary_key();
		deposit.usd = this->eos2usd(quantity, this->state.eosusd);
	});

	deposit deposit_table(this->_self, this->_self);
	auto it = deposit_table.find(investor);
	if (it == deposit_table.end()) {
		deposit_table.emplace(this->_self, [&](auto& deposit) {
			deposit.account = investor;
			deposit.usd = this->eos2usd(quantity, this->state.eosusd);
			deposit.eos = quantity;
		});
	} else {
		deposit_table.modify(it, this->_self, [&](auto& deposit) {
			deposit.usd += this->eos2usd(quantity, this->state.eosusd);
			deposit.eos += quantity;
		});
	}
}

void crowdsale::init(time_t start, time_t finish) {
	eosio_assert(!this->state_singleton.exists(), "Already initialized");
	eosio_assert(start < finish, "Start must be less than finish");

	require_auth(this->issuer);

	this->state.start = start;
	this->state.finish = finish;

	#define PREMINT(z, i, data) \
		this->inline_issue(\
			eosio::string_to_name(STR(MINTDEST ## i)),\
			ASSET_TKN(MINTVAL ## i),\
			"Initial token distribution"\
		);
	BOOST_PP_REPEAT(MINTCNT, PREMINT, );
}

void crowdsale::setstart(time_t start) {
	require_auth(this->issuer);
	eosio_assert(this->state_singleton.exists(), "Not initialized");
	eosio_assert(NOW <= this->state.start, "Crowdsale already started");
	eosio_assert(start < this->state.finish, "Start must be less than finish");
	this->state.start = start;
}

void crowdsale::setfinish(time_t finish) {
	require_auth(this->issuer);
	eosio_assert(this->state_singleton.exists(), "Not initialized");
	eosio_assert(NOW <= this->state.finish, "Crowdsale finished");
	eosio_assert(finish > this->state.start, "Finish must be greater than start");
	this->state.finish = finish;
}

void crowdsale::white(account_name account) {
	require_auth(this->issuer);
	eosio_assert(this->state_singleton.exists(), "Not initialized");
	eosio_assert(!this->state.finished && !this->state.hardcap_reached, "Crowdsale finished");
	this->setwhite(account);
}

void crowdsale::whitemany(eosio::vector<account_name> accounts) {
	require_auth(this->issuer);
	eosio_assert(this->state_singleton.exists(), "Not initialized");
	eosio_assert(!this->state.finished && !this->state.hardcap_reached, "Crowdsale finished");
	for (account_name account : accounts) {
		this->setwhite(account);
	}
}

void crowdsale::withdraw(account_name investor) {
	require_auth(investor);

	eosio_assert(this->state_singleton.exists(), "Not initialized");
	eosio_assert(this->state.finished || this->state.hardcap_reached, "Crowdsale not finished");
	eosio_assert(this->whitelist.find(investor) != this->whitelist.end(), "Not whitelisted, call refund");

	deposit deposit_table(this->_self, this->_self);
	auto deposit_it = deposit_table.find(investor);
	eosio_assert(deposit_it != deposit_table.end(), "Nothing to withdraw");

	eosio::extended_asset community_eos = this->state.total_eos - this->usd2eos(ASSET_USD(HARD_CAP_USD * this->eos2usd(this->state.total_eos, this->state.eosusd).amount / this->total_usd().amount), this->state.eosusd);
	eosio::extended_asset tkn = ASSET_TKN(0);
	eosio::extended_asset eos = ASSET_EOS(community_eos.amount * deposit_it->eos.amount / this->state.total_eos.amount);

	double rate = 1.0;
	if (this->state.hardcap_reached) {
		rate *= HARD_CAP_USD;
		rate /= this->total_usd().amount;
	}

	deposits deposits_table(this->_self, investor);
	for (auto it = deposits_table.begin(); it != deposits_table.end();) {
		eosio::asset part = this->usd2tkn(it->usd);
		part.amount *= rate;
		tkn += part;
		it = deposits_table.erase(it);
	}

	this->inline_issue(investor, tkn, "Crowdsale");
	if (this->state.hardcap_reached && eos.amount > 0) {
		this->inline_transfer(this->_self, investor, eos, "Crowdsale");
	}
}

void crowdsale::refund(account_name investor) {
	require_auth(investor);

	eosio_assert(this->state_singleton.exists(), "Not initialized");

	auto it = this->whitelist.find(investor);
	eosio_assert(it == this->whitelist.end(), "No pending investments");

	eosio::extended_asset eoses = ASSET_EOS(0);

	deposits deposits_table(this->_self, investor);
	for (auto it = deposits_table.begin(); it != deposits_table.end();) {
		it = deposits_table.erase(it);
	}

	deposit deposit_table(this->_self, this->_self);
	auto deposit_it = deposit_table.find(investor);
	eosio_assert(deposit_it != deposit_table.end(), "Nothing to refund");

	this->inline_transfer(this->_self, investor, deposit_it->eos, "Refund");
}

void crowdsale::finalize() {
	require_auth(this->issuer);

	eosio_assert(this->state_singleton.exists(), "Not initialized");
	eosio_assert(this->state.finished || this->state.hardcap_reached, "Crowdsale not finished");
	eosio_assert(!this->state.finalized, "Already finalized");

	eosio::extended_asset eos;
	if (this->state.hardcap_reached) {
		eos = this->usd2eos(ASSET_USD(HARD_CAP_USD * this->eos2usd(this->state.total_eos, this->state.eosusd).amount / this->total_usd().amount), this->state.eosusd);
	} else {
		eos = this->state.total_eos;
	}

	this->inline_transfer(this->_self, this->issuer, eos, "Finalize");

	this->state.finalized = true;
}

void crowdsale::setdaily(eosio::asset usdoneth, eosio::asset eosusd, time_t next_update) {
	require_auth(this->issuer);

	eosio_assert(this->state_singleton.exists(), "Not initialized");
	eosio_assert(usdoneth.symbol == SYMBOL_USD, "Invalid USD symbol for USD raised on ETH contract");
	eosio_assert(eosusd.symbol == SYMBOL_USD, "Invalid USD symbol for EOSUSD");

	eosio_assert(NOW >= this->state.valid_until, "Rates are already updated");
	eosio_assert(!this->state.finished && !this->state.hardcap_reached, "Crowdsale finished");

	this->state.usdoneth = usdoneth;
	this->state.eosusd = eosusd;

	if (NOW > this->state.finish) {
		this->state.finished = true;
	}

	if (this->total_usd().amount > HARD_CAP_USD) {
		this->state.hardcap_reached = true;
	}

	this->state.valid_until = NOW + next_update;
}

void crowdsale::cleanstate() {
	this->toclean = true;
}

#ifdef DEBUG
void crowdsale::settime(time_t time) {
	eosio_assert(this->state_singleton.exists(), "Not initialized");
	this->state.time = time;
}
EOSIO_ABI(crowdsale, (init)(setstart)(setfinish)(white)(whitemany)(withdraw)(refund)(finalize)(setdaily)(transfer)(cleanstate)(settime));
#else
EOSIO_ABI(crowdsale, (init)(setstart)(setfinish)(white)(whitemany)(withdraw)(refund)(finalize)(setdaily)(transfer)(cleanstate));
#endif
