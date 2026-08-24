// Copyright (c) 2014-2020 Sebastien Rombauts (sebastien.rombauts@gmail.com)
//
// Distributed under the MIT License (MIT) (See accompanying file LICENSE.txt
// or copy at http://opensource.org/licenses/MIT)

#pragma once

#include "Widgets/SCompoundWidget.h"

class SGitSourceControlSettings : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SGitSourceControlSettings) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

private:
	FText GetGitBinaryPath() const;
	FText GetPathToRepositoryRoot() const;
};
