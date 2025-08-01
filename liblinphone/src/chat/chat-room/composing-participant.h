/*
 * Copyright (c) 2010-2025 Belledonne Communications SARL.
 *
 * This file is part of Liblinphone
 * (see https://gitlab.linphone.org/BC/public/liblinphone).
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef _L_COMPOSING_PARTICIPANT_H_
#define _L_COMPOSING_PARTICIPANT_H_

#include "c-wrapper/c-wrapper.h"
#include "linphone/api/c-types.h"

// =============================================================================

LINPHONE_BEGIN_NAMESPACE

class ComposingParticipant : public bellesip::HybridObject<LinphoneComposingParticipant, ComposingParticipant> {
public:
	ComposingParticipant(const std::shared_ptr<Address> &address, const std::string &contentType);
	ComposingParticipant(const ComposingParticipant &other);
	~ComposingParticipant();

	ComposingParticipant *clone() const override;

	const std::shared_ptr<Address> &getAddress() const;

	const std::string &getContentType() const;

private:
	std::shared_ptr<Address> mAddress;
	std::string mContentType;
};

LINPHONE_END_NAMESPACE

#endif // ifndef _L_COMPOSING_PARTICIPANT_H_
