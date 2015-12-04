module Notgios

  class Nodis < Redis

    UserExistsError = Class.new(StandardError)
    NoSuchUserError = Class.new(StandardError)

    def get_users
      smembers('notgios.users')
    end

    def add_user(username, pass)
      if !exists("notgios.users.#{username}")
        salt = SecureRandom.hex(16)
        crypted = Digest::SHA256.hexdigest("#{pass}|#{salt}")
        multi do
          hset("notgios.users.#{username}", 'password', crypted)
          hset("notgios.users.#{username}", 'salt', salt)
          sadd("notgios.users", username)
        end
      else
        raise UserExistsError, "User #{username} already exists."
      end
    end

    def authenticate_user(username, pass)
      if exists("notgios.users.#{username}")
        salt = hget("notgios.users.#{username}", 'salt')
        crypted = hget("notgios.users.#{username}, 'crypted'")
        crypted == Digest::SHA256.hexdigest("#{pass}|#{salt}")
      else
        raise NoSuchUserError, "User #{username} does not exist."
      end
    end

    def user_exists?(username)
      exists("notgios.users.#{username}")
    end

    # FIXME: Revisit this in the future to make sure it actually clears all user data.
    def delete_user(username)
      multi do
        srem("notgios.users", username)
        del("notgios.users.#{username}")
        del("notgios.servers.#{username}")
        del("notgios.jobs.#{username}")
      end
    end

  end
end
