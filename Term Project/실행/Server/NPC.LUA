myid = 9999
count = 0
event = false

function set_uid(x)
myid = x
end

function event_player_move(p_id)
	if API_get_x(p_id) == API_get_x(myid) then
		if API_get_y(p_id) == API_get_y(myid) then
			API_send_message(myid, player, "Monster Attack!")
		end
	end
end

function event_npc()
	count = count + 1
	if count > 2 then
		API_send_message(myid, player, "BYE")
		count = 0
		event = false
	end
end