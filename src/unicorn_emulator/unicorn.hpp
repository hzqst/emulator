#pragma once

#pragma warning(push)
#pragma warning(disable: 4505)
#define NOMINMAX
#include <unicorn/unicorn.h>
#pragma warning(pop)

#include <stdexcept>

namespace unicorn
{
	struct unicorn_error : std::runtime_error
	{
		unicorn_error(const uc_err error_code)
			: std::runtime_error(uc_strerror(error_code))
			  , code(error_code)
		{
		}

		uc_err code{};
	};

	inline void throw_if_unicorn_error(const uc_err error_code)
	{
		if (error_code != UC_ERR_OK)
		{
			throw unicorn_error(error_code);
		}
	}

	inline void uce(const uc_err error_code)
	{
		throw_if_unicorn_error(error_code);
	}
}